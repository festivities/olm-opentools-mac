/* OLMDirectionalBlur.cpp - macOS port of OLM Directional Blur 1.1.1.
 *
 * Rewritten 1:1 from the Windows x64 binary after full decompilation
 * (see .agents/AGENTS.md). All math, buffer layouts, defaults, flags and
 * UI behaviour below mirror the Windows original; only platform glue
 * differs (EffectMain entry, AEGP_SuiteHandler construction).
 *
 * Windows original pipeline (per format: 8/16/32 bit):
 *   1. PF_iterate over output extent: convert input world -> RGBA float canvas
 *      (canvas = diagonal-sized square, input placed at margins mx/my).
 *   2. mode==Layer: build premultiplied-luma plane of noise layer (canvas
 *      positioned by layer origins, bounds-checked); mode==Smooth/Block:
 *      build a random lattice field (MT19937, 101-entry table, cell size =
 *      Thickness).
 *   3. rotate canvas by +angle (alpha-weighted bilinear; plane rotated too).
 *   4. copy rotated canvas back over the source canvas.
 *   5. if (Sharp Tail != 0 or Size Variation != 0): run-length connected
 *      components of (alpha>0) mask -> per-pixel stats [area, minRow,
 *      midRow, maxRow-midRow] (maxRow starts at 0, single-run components
 *      therefore get negative extents - binary-faithful quirk) and
 *      maxlevel = max component area.
 *   6. Gaussian LUTs: blur strength (Blur Strength) and window LUTs
 *      (Alpha Fade) per direction, exp(-i^2/(2*(n/3)^2 + 1e-5)).
 *   7. blur core (serial, rows in up to 32 chunks like the original):
 *        size pass (windowed alpha normalization using Alpha Fade LUTs,
 *        window reach = fade * pow(area/maxlevel, Size Variation)),
 *        then front/back scatter passes (Blur Strength LUTs, reach =
 *        blur * weight, weight *= fmaxf(0, 1 - |y-mid|*tail/half) and
 *        noise lerp(1, noise, Noise Variation)).
 *   8. normalize RGB by accumulated weight; zero source canvas.
 *   9. rotate canvas by -angle.
 *  10. PF_iterate over output extent: write output (RGB * Brightness Gain
 *      clamped to 1, alpha raw; integer formats converted by truncation).
 */

#include "OLMDirectionalBlur.h"

#include <math.h>

#include <cmath>
#include <cstring>
#include <new>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

//============================================================= MT19937 ======
// Original uses std::mt19937 (init_genrand(seed), genrand_int32() * 2^-32,
// zero-extended - verified in disassembly of sub_1800026B0).
struct Mt {
    unsigned int state[624];
    int index;
    Mt() : index(625) {}
    void seed(unsigned int s) {
        state[0] = s;
        for (int i = 1; i < 624; ++i)
            state[i] = 1812433253u * (state[i - 1] ^ (state[i - 1] >> 30)) + (unsigned int)i;
        index = 624;
    }
    unsigned int next() {
        if (index >= 624) {
            if (index == 625) seed(5489u);  // uninitialized default, as original
            for (int i = 0; i < 624; ++i) {
                unsigned int y = (state[i] & 0x80000000u) | (state[(i + 1) % 624] & 0x7fffffffu);
                state[i] = state[(i + 397) % 624] ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
            }
            index = 0;
        }
        unsigned int y = state[index++];
        y ^= y >> 11;
        y ^= (y << 7) & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        y ^= y >> 18;
        return y;
    }
    double u01() { return (double)next() * 2.3283064365386963e-10; }
};

//=========================================================== noise field =====
struct NoiseField {
    int gw = 0, gh = 0;
    float cell = 1.0f;
    std::vector<float> table;  // 101 entries in [-1,1]
    std::vector<float> field;
};

void build_noise_field(NoiseField &nf, int W, int H, float thickness, float offset, int seed) {
    nf.cell = thickness;
    nf.gw = (int)((float)W / thickness + 3.0f);
    nf.gh = (int)((float)H / thickness + 3.0f);
    Mt rng;
    rng.seed((unsigned int)seed);
    nf.table.resize(101);
    for (int i = 0; i < 101; ++i)
        nf.table[i] = (float)(rng.u01() * 2.0 - 1.0);
    nf.field.assign((size_t)nf.gw * (size_t)nf.gh, 0.0f);
    for (int y = 0; y < nf.gh; ++y) {
        for (int x = 0; x < nf.gw; ++x) {
            float white = (float)rng.u01();
            float t = (float)(rng.u01() * 100.0 + (double)offset);
            while (t >= 100.0f) t -= 100.0f;
            if (t < 0.0f) t += 100.0f;  // safety; original indexes OOB for negative offsets
            int i0 = (int)t;
            float f = t - (float)i0;
            float f2 = f * f;
            float s = f2 * (3.0f - (f + f));
            float v = ((1.0f - s) * nf.table[i0] + s * nf.table[i0 + 1]) * 0.5f + white;
            nf.field[(size_t)y * (size_t)nf.gw + (size_t)x] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        }
    }
}

// Nearest (Block) / smoothstep-bilinear (Smooth) sampling, sub_180003370.
float sample_noise(const NoiseField &nf, int x, int y, bool smooth) {
    float gx = (float)x / nf.cell;
    float gy = (float)y / nf.cell;
    int ix = (int)gx;
    int iy = (int)gy;
    if (!smooth)
        return nf.field[(size_t)iy * (size_t)nf.gw + (size_t)ix];
    float fx = gx - (float)ix;
    float fy = gy - (float)iy;
    float wx = fx * fx * (3.0f - (fx + fx));
    float wy = fy * fy * (3.0f - (fy + fy));
    const float *f = nf.field.data();
    size_t i00 = (size_t)iy * (size_t)nf.gw + (size_t)ix;
    return ((1.0f - wy) * wx) * f[i00 + 1] +
           ((1.0f - wx) * (1.0f - wy)) * f[i00] +
           ((1.0f - wx) * wy) * f[i00 + (size_t)nf.gw] +
           (wy * wx) * f[i00 + (size_t)nf.gw + 1];
}

//======================================================== Gaussian LUTs =====
// sub_180001830: lut[i] = exp(-i^2 / (2*(n/3)^2 + 1e-5)), n = param value.
void build_lut(std::vector<float> &lut, int n) {
    lut.resize((size_t)n);
    float sigma = (float)n / 3.0f;
    float sigma_sq = sigma * sigma;
    float denom = (float)((double)sigma_sq + (double)sigma_sq + 0.00001);
    for (int i = 0; i < n; ++i)
        lut[(size_t)i] = expf((float)(-(i * i)) / denom);
}

//=========================================================== rotations ======
// Inverse-mapped bilinear rotate around image centre, strictly-interior guard
// (borders keep their previous content - buffers are pre-zeroed).
// RGBA: alpha-weighted colour (sub_180001EC0); plane: plain bilinear
// (sub_1800018C0). Mapping: sy = (x-cx)*s + (y-cy)*c + cy, sx = (x-cx)*c -
// (y-cy)*s + cx.
void rotate_rgba(const float *src, float *dst, int w, int h, float ang) {
    const int cx = w / 2, cy = h / 2;
    const float c = cosf(ang), s = sinf(ang);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float sy = ((float)(x - cx) * s + (float)(y - cy) * c) + (float)cy;
            float sx = ((float)(x - cx) * c - (float)(y - cy) * s) + (float)cx;
            int ix = (int)sx, iy = (int)sy;
            if (ix > 0 && ix < w - 1 && iy > 0 && iy < h - 1) {
                float fx = sx - (float)ix, fy = sy - (float)iy;
                const float *p00 = src + ((size_t)iy * (size_t)w + (size_t)ix) * 4u;
                const float *p10 = p00 + 4;
                const float *p01 = p00 + (size_t)w * 4u;
                const float *p11 = p01 + 4;
                float a00 = p00[3], a10 = p10[3], a01 = p01[3], a11 = p11[3];
                float w00 = (1.0f - fx) * (1.0f - fy) * a00;
                float w10 = fx * (1.0f - fy) * a10;
                float w01 = (1.0f - fx) * fy * a01;
                float w11 = fx * fy * a11;
                float asum = ((w00 + w10) + w01) + w11;
                if (asum != 0.0f) {
                    w00 /= asum; w10 /= asum; w01 /= asum; w11 /= asum;
                }
                float *d = dst + ((size_t)y * (size_t)w + (size_t)x) * 4u;
                d[0] = ((p00[0] * w00 + p10[0] * w10) + p01[0] * w01) + p11[0] * w11;
                d[1] = ((p00[1] * w00 + p10[1] * w10) + p01[1] * w01) + p11[1] * w11;
                d[2] = ((p00[2] * w00 + p10[2] * w10) + p01[2] * w01) + p11[2] * w11;
                d[3] = asum;
            }
        }
    }
}

void rotate_plane(const float *src, float *dst, int w, int h, float ang) {
    const int cx = w / 2, cy = h / 2;
    const float c = cosf(ang), s = sinf(ang);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float sy = ((float)(x - cx) * s + (float)(y - cy) * c) + (float)cy;
            float sx = ((float)(x - cx) * c - (float)(y - cy) * s) + (float)cx;
            int ix = (int)sx, iy = (int)sy;
            if (ix > 0 && ix < w - 1 && iy > 0 && iy < h - 1) {
                float fx = sx - (float)ix, fy = sy - (float)iy;
                const float *p = src + (size_t)iy * (size_t)w + (size_t)ix;
                dst[(size_t)y * (size_t)w + (size_t)x] =
                    ((1.0f - fy) * (1.0f - fx)) * p[0] +
                    ((1.0f - fy) * fx) * p[1] +
                    fy * (1.0f - fx) * p[(size_t)w] +
                    (fy * fx) * p[(size_t)w + 1];
            }
        }
    }
}

//===================================================== working context =======
struct Ctx {
    int W = 0, H = 0;    // canvas size
    int mx = 0, my = 0;  // margins (input origin inside canvas)
    int iw = 0, ih = 0;  // input world size
    std::vector<float> A, B;         // RGBA canvases
    std::vector<float> wsum, alpha;  // 1 float per pixel
    std::vector<float> comp;         // 4 floats per pixel
    std::vector<unsigned char> mask;
    std::vector<float> nzin, nzrot;  // noise planes (Layer mode)
    bool have_noise_plane = false;
    std::vector<float> lut_front_blur, lut_back_blur, lut_front_fade, lut_back_fade;
    float maxlevel = 0.0f;
    OLMDBParams p;
    NoiseField nf;

    size_t pix(int x, int y) const { return (size_t)y * (size_t)W + (size_t)x; }
    float *px(std::vector<float> &buf, int x, int y) { return &buf[pix(x, y) * 4u]; }
    const float *px(const std::vector<float> &buf, int x, int y) const { return &buf[pix(x, y) * 4u]; }
};

//==================================================== component analysis =====
// Run-length connected components over the (alpha>0) mask, cross-linked only
// between adjacent rows (sub_1800028E0 + helpers). Writes 4 floats per pixel:
// [area, minRow, (minRow+maxRow)/2, maxRow-mid], maxRow initialised to 0.
void component_pass(Ctx &c) {
    const int W = c.W, H = c.H;
    std::vector<int> row_start((size_t)H + 1, 0);

    struct Run { int row, x0, x1, len; };
    std::vector<Run> runs;
    runs.reserve(1024);
    for (int y = 0; y < H; ++y) {
        row_start[(size_t)y] = (int)runs.size();
        int x = 0;
        while (x < W) {
            if (c.mask[(size_t)y * (size_t)W + (size_t)x]) {
                int x0 = x;
                while (x + 1 < W && c.mask[(size_t)y * (size_t)W + (size_t)(x + 1)]) ++x;
                runs.push_back(Run{y, x0, x, x - x0 + 1});
            }
            ++x;
        }
    }
    row_start[(size_t)H] = (int)runs.size();
    const int nruns = (int)runs.size();
    if (nruns == 0) {
        c.maxlevel = 0.0f;
        return;
    }

    // Adjacency: overlapping runs in consecutive rows, links in both directions.
    std::vector<int> link_count((size_t)nruns, 0);
    auto overlaps = [](const Run &a, const Run &b) { return a.x0 <= b.x1 && b.x0 <= a.x1; };
    for (int y = 1; y < H; ++y) {
        for (int i = row_start[(size_t)y]; i < row_start[(size_t)y + 1]; ++i) {
            for (int j = row_start[(size_t)y - 1]; j < row_start[(size_t)y]; ++j) {
                if (overlaps(runs[(size_t)i], runs[(size_t)j])) {
                    ++link_count[(size_t)i];
                    ++link_count[(size_t)j];
                }
            }
        }
    }
    std::vector<int> adj_begin((size_t)nruns + 1, 0);
    for (int i = 0; i < nruns; ++i)
        adj_begin[(size_t)i + 1] = adj_begin[(size_t)i] + link_count[(size_t)i];
    std::vector<int> adj((size_t)adj_begin[(size_t)nruns]);
    std::vector<int> fill_pos(adj_begin.begin(), adj_begin.end() - 1);
    for (int y = 1; y < H; ++y) {
        for (int i = row_start[(size_t)y]; i < row_start[(size_t)y + 1]; ++i) {
            for (int j = row_start[(size_t)y - 1]; j < row_start[(size_t)y]; ++j) {
                if (overlaps(runs[(size_t)i], runs[(size_t)j])) {
                    adj[(size_t)fill_pos[(size_t)i]++] = j;
                    adj[(size_t)fill_pos[(size_t)j]++] = i;
                }
            }
        }
    }

    std::vector<unsigned char> visited((size_t)nruns, 0);
    std::vector<int> stack, members;
    float maxlevel = 0.0f;
    for (int r = 0; r < nruns; ++r) {
        if (visited[(size_t)r]) continue;
        stack.clear();
        members.clear();
        stack.push_back(r);
        visited[(size_t)r] = 1;
        int area = 0, minrow = H, maxrow = 0;  // maxrow starts at 0 (binary-faithful)
        while (!stack.empty()) {
            int id = stack.back();
            stack.pop_back();
            members.push_back(id);
            area += runs[(size_t)id].len;
            int row = runs[(size_t)id].row;
            if (minrow <= row) {
                if (maxrow < row) maxrow = row;
            } else {
                minrow = row;
            }
            for (int k = adj_begin[(size_t)id]; k < adj_begin[(size_t)id + 1]; ++k) {
                int ch = adj[(size_t)k];
                if (!visited[(size_t)ch]) {
                    visited[(size_t)ch] = 1;
                    stack.push_back(ch);
                }
            }
        }
        float mid = (float)((minrow + maxrow) / 2);
        float half = (float)maxrow - mid;
        for (size_t m = 0; m < members.size(); ++m) {
            const Run &run = runs[(size_t)members[m]];
            for (int x = run.x0; x <= run.x1; ++x) {
                float *st = c.px(c.comp, x, run.row);
                st[0] = (float)area;
                st[1] = (float)minrow;
                st[2] = mid;
                st[3] = half;
            }
        }
        if ((float)area > maxlevel) maxlevel = (float)area;
    }
    c.maxlevel = maxlevel;
}

//================================================== size & scatter passes =====
// sub_180001000: windowed alpha normalization. Uses the Alpha Fade LUTs;
// reaches are (int)(fade * weight) clamped to the row; forward taps read
// alpha(x+k), backward alpha(x-k), k = 1..reach-1. Writes premultiplied RGBA
// and v71 into wsum/alpha buffers (all later summed by the scatter passes).
void size_pass(Ctx &c, int x, int y, float weight) {
    const size_t idx = c.pix(x, y);
    float *dst = &c.B[idx * 4u];
    const float *src = &c.A[idx * 4u];
    float a = src[3];
    if (a == 0.0f) {
        dst[0] = dst[1] = dst[2] = dst[3] = 0.0f;
        c.wsum[idx] = 0.0f;
        c.alpha[idx] = 0.0f;
        return;
    }
    float inv = 1.0f;
    if (weight > 0.0f) inv = 1.0f / weight;

    // reach = (x + (int)(fade*w) < W) ? (int)(fade*w) : W-x  (binary order)
    int rf = (int)((float)c.p.front_fade * weight);
    int reach_f = (x + rf < c.W) ? rf : (c.W - x);
    int rb = (int)((float)c.p.back_fade * weight);
    int reach_b = (x - rb < 0) ? x : rb;

    float sum = 1.0f;
    float asum = a;
    for (int k = 1; k < reach_f; ++k) {
        float f = c.lut_front_fade[(size_t)(int)((float)k * inv)];
        sum += f;
        asum += f * c.A[c.pix(x + k, y) * 4u + 3u];   // alpha(pix + k)
    }
    for (int k = 1; k < reach_b; ++k) {
        float f = c.lut_back_fade[(size_t)(int)((float)k * inv)];
        sum += f;
        asum += f * c.A[c.pix(x - k, y) * 4u + 3u];   // alpha(pix - k)
    }
    float v71 = asum / sum;
    dst[0] = v71 * src[0];
    dst[1] = v71 * src[1];
    dst[2] = v71 * src[2];
    dst[3] = v71;
    c.wsum[idx] = v71;
    c.alpha[idx] = v71;
}

// sub_1800013E0: scatter centre RGB along x; destination alpha takes the max
// scatter weight, wsum accumulates the weights. dir_neg = towards -x.
void splat_pass(Ctx &c, int x, int y, bool dir_neg, const float *lut, int len, float weight) {
    if (weight <= 0.0f) return;
    int reach = (int)((float)len * weight);
    if (reach < 2) return;
    const int step = dir_neg ? -1 : 1;
    if (dir_neg) {
        if (x - reach < 0) reach = x;
    } else {
        if (x + reach >= c.W) reach = c.W - x;
    }
    const float inv = 1.0f / weight;
    const size_t idx = c.pix(x, y);
    const float *src = &c.A[idx * 4u];
    const float a_center = c.alpha[idx];
    for (int k = 1; k < reach; ++k) {
        float w = a_center * lut[(size_t)(int)((float)k * inv)];
        size_t t = idx + (size_t)(step * k);
        float *d = &c.B[t * 4u];
        d[0] += w * src[0];
        d[1] += w * src[1];
        d[2] += w * src[2];
        if (w > d[3]) d[3] = w;
        c.wsum[t] += w;
    }
}

// sub_1800038D0 (blur core), one row at a time. Serial: the Windows release
// build calls omp_get_max_threads() but contains no VCOMP fork, i.e. the
// shipped binary is serial; chunking rows therefore changes nothing.
void blur_row(Ctx &c, int y) {
    const OLMDBParams &p = c.p;
    for (int x = 0; x < c.W; ++x) {
        float w = powf(c.comp[c.pix(x, y) * 4u] / c.maxlevel, p.size_var);
        size_pass(c, x, y, w);
    }
    for (int x = 0; x < c.W; ++x) {
        const size_t idx = c.pix(x, y);
        if (c.B[idx * 4u + 3u] == 0.0f) continue;
        float w = powf(c.comp[idx * 4u] / c.maxlevel, p.size_var);
        float factor = 1.0f;
        if (p.mode == OLMDB_MODE_LAYER) {
            if (c.have_noise_plane) {
                float nz = c.nzrot[idx];
                factor = (1.0f - p.noise_var) + p.noise_var * nz;
            }
        } else if (p.mode == OLMDB_MODE_GENERATED) {
            float nz = sample_noise(c.nf, x, y, p.smooth);
            factor = (1.0f - p.noise_var) + p.noise_var * nz;
        }
        float v28 = w * factor;
        if (v28 == 0.0f) continue;
        const float *st = &c.comp[idx * 4u];
        float f_front = fmaxf(0.0f, 1.0f - (fabsf((float)y - st[2]) * p.front_tail) / st[3]);
        float f_back = fmaxf(0.0f, 1.0f - (fabsf((float)y - st[2]) * p.back_tail) / st[3]);
        splat_pass(c, x, y, true, c.lut_front_blur.data(), (int)c.lut_front_blur.size(), f_front * v28);
        splat_pass(c, x, y, false, c.lut_back_blur.data(), (int)c.lut_back_blur.size(), f_back * v28);
    }
}

//========================================================= param checkout =====
short fixed_short(PF_Fixed v) { return (short)(v >> 16); }  // SWORD1

PF_Err checkout_params(PF_InData *in_data, OLMDBParams *p) {
    PF_Err err = PF_Err_NONE;
    PF_ParamDef cur;

    memset(p, 0, sizeof(*p));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_ANGLE, in_data->current_time, in_data->time_step,
                          in_data->time_scale, &cur));
    // Binary's PI literal is the double 3.14159265 (0x400921FB53C8D4F1), not full precision.
    p->angle_rad = (fixed_short(cur.u.ad.value) + 90.0) / 180.0 * 3.14159265;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_BRIGHTNESS, in_data->current_time, in_data->time_step,
                          in_data->time_scale, &cur));
    p->brightness = (float)cur.u.fs_d.value;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_SIZE_VAR, in_data->current_time, in_data->time_step,
                          in_data->time_scale, &cur));
    p->size_var = (float)fixed_short(cur.u.fd.value) / 100.0f;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_FRONT_BLUR, in_data->current_time, in_data->time_step,
                          in_data->time_scale, &cur));
    p->front_blur = cur.u.sd.value;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_FRONT_FADE, in_data->current_time, in_data->time_step,
                          in_data->time_scale, &cur));
    p->front_fade = cur.u.sd.value;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_FRONT_TAIL, in_data->current_time, in_data->time_step,
                          in_data->time_scale, &cur));
    p->front_tail = (float)fixed_short(cur.u.fd.value) / 100.0f;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_BACK_BLUR, in_data->current_time, in_data->time_step,
                          in_data->time_scale, &cur));
    p->back_blur = cur.u.sd.value;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_BACK_FADE, in_data->current_time, in_data->time_step,
                          in_data->time_scale, &cur));
    p->back_fade = cur.u.sd.value;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_BACK_TAIL, in_data->current_time, in_data->time_step,
                          in_data->time_scale, &cur));
    p->back_tail = (float)fixed_short(cur.u.fd.value) / 100.0f;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_NOISE_VAR, in_data->current_time, in_data->time_step,
                          in_data->time_scale, &cur));
    p->noise_var = (float)fixed_short(cur.u.fd.value) / 100.0f;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_NOISE_TYPE, in_data->current_time, in_data->time_step,
                          in_data->time_scale, &cur));
    p->noise_type = cur.u.pd.value;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));
    p->smooth = (p->noise_type == OLMDB_POPUP_SMOOTH);

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_SEED, in_data->current_time, in_data->time_step,
                          in_data->time_scale, &cur));
    p->seed = cur.u.sd.value;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_OFFSET, in_data->current_time, in_data->time_step,
                          in_data->time_scale, &cur));
    p->offset = (float)fixed_short(cur.u.ad.value) / 36.0f;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_THICKNESS, in_data->current_time, in_data->time_step,
                          in_data->time_scale, &cur));
    p->thickness = (float)cur.u.fs_d.value;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    p->mode = OLMDB_MODE_OFF;
    if (p->noise_var > 0.0f)
        p->mode = (p->noise_type != OLMDB_POPUP_LAYER) ? OLMDB_MODE_GENERATED : OLMDB_MODE_LAYER;
    return err;
}

}  // namespace

//======================================================= iterate callbacks =====
// Format-specific conversion between the host worlds and the RGBA float canvas.
// x/y are layer coordinates; the input is placed at (mx,my) inside the canvas.

static PF_Err cb_read8(void *ref, A_long x, A_long y, PF_Pixel8 *in, PF_Pixel8 *out) {
    (void)out;
    Ctx *c = (Ctx *)ref;
    float *d = c->px(c->A, (int)x + c->mx, (int)y + c->my);
    d[0] = (float)in->red / 255.0f;
    d[1] = (float)in->green / 255.0f;
    d[2] = (float)in->blue / 255.0f;
    d[3] = (float)in->alpha / 255.0f;
    return PF_Err_NONE;
}
static PF_Err cb_read16(void *ref, A_long x, A_long y, PF_Pixel16 *in, PF_Pixel16 *out) {
    (void)out;
    Ctx *c = (Ctx *)ref;
    float *d = c->px(c->A, (int)x + c->mx, (int)y + c->my);
    d[0] = (float)in->red / 32768.0f;
    d[1] = (float)in->green / 32768.0f;
    d[2] = (float)in->blue / 32768.0f;
    d[3] = (float)in->alpha / 32768.0f;
    return PF_Err_NONE;
}
static PF_Err cb_read32(void *ref, A_long x, A_long y, PF_PixelFloat *in, PF_PixelFloat *out) {
    (void)out;
    Ctx *c = (Ctx *)ref;
    float *d = c->px(c->A, (int)x + c->mx, (int)y + c->my);
    d[0] = in->red;
    d[1] = in->green;
    d[2] = in->blue;
    d[3] = in->alpha;
    return PF_Err_NONE;
}

static PF_Err cb_write8(void *ref, A_long x, A_long y, PF_Pixel8 *in, PF_Pixel8 *out) {
    (void)in;
    Ctx *c = (Ctx *)ref;
    const float *s = c->px(c->A, (int)x + c->mx, (int)y + c->my);
    const float br = c->p.brightness;
    out->red = (A_u_char)(int)(fminf(br * s[0], 1.0f) * 255.0f);
    out->green = (A_u_char)(int)(fminf(br * s[1], 1.0f) * 255.0f);
    out->blue = (A_u_char)(int)(fminf(br * s[2], 1.0f) * 255.0f);
    out->alpha = (A_u_char)(int)(s[3] * 255.0f);
    return PF_Err_NONE;
}
static PF_Err cb_write16(void *ref, A_long x, A_long y, PF_Pixel16 *in, PF_Pixel16 *out) {
    (void)in;
    Ctx *c = (Ctx *)ref;
    const float *s = c->px(c->A, (int)x + c->mx, (int)y + c->my);
    const float br = c->p.brightness;
    out->red = (A_u_short)(int)(fminf(br * s[0], 1.0f) * 32768.0f);
    out->green = (A_u_short)(int)(fminf(br * s[1], 1.0f) * 32768.0f);
    out->blue = (A_u_short)(int)(fminf(br * s[2], 1.0f) * 32768.0f);
    out->alpha = (A_u_short)(int)(s[3] * 32768.0f);
    return PF_Err_NONE;
}
static PF_Err cb_write32(void *ref, A_long x, A_long y, PF_PixelFloat *in, PF_PixelFloat *out) {
    (void)in;
    Ctx *c = (Ctx *)ref;
    const float *s = c->px(c->A, (int)x + c->mx, (int)y + c->my);
    const float br = c->p.brightness;
    out->red = fminf(br * s[0], 1.0f);
    out->green = fminf(br * s[1], 1.0f);
    out->blue = fminf(br * s[2], 1.0f);
    out->alpha = s[3];
    return PF_Err_NONE;
}

namespace {

//=============================================================== pipeline =====
PF_Err iterate_pass(PF_InData *in_data, PF_OutData *out_data, int bitdepth, bool read_pass,
                    PF_EffectWorld *src, PF_Rect *area, Ctx *ctx, PF_EffectWorld *dst,
                    A_long progress_final) {
    PF_Err err = PF_Err_NONE;
    if (bitdepth == 8) {
        AEFX_SuiteScoper<PF_Iterate8Suite1> s(in_data, kPFIterate8Suite, kPFIterate8SuiteVersion1,
                                             out_data);
        err = s->iterate(in_data, 0, progress_final, src, area, ctx,
                         read_pass ? cb_read8 : cb_write8, dst);
    } else if (bitdepth == 16) {
        AEFX_SuiteScoper<PF_Iterate16Suite1> s(in_data, kPFIterate16Suite, kPFIterate16SuiteVersion1,
                                               out_data);
        err = s->iterate(in_data, 0, progress_final, src, area, ctx,
                         read_pass ? cb_read16 : cb_write16, dst);
    } else {
        AEFX_SuiteScoper<PF_IterateFloatSuite1> s(in_data, kPFIterateFloatSuite,
                                                  kPFIterateFloatSuiteVersion1, out_data);
        err = s->iterate(in_data, 0, progress_final, src, area, ctx,
                         read_pass ? cb_read32 : cb_write32, dst);
    }
    return err;
}

PF_Err render_format(PF_InData *in_data, PF_OutData *out_data, PF_EffectWorld *input,
                     PF_EffectWorld *output, PF_PixelFormat fmt, int bitdepth, OLMDBParams &p) {
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    Ctx ctx;
    ctx.p = p;
    ctx.iw = input->width;
    ctx.ih = input->height;

    // Canvas size: diagonal square + 2px, input centred (binary formulas).
    {
        int d2 = ctx.iw * ctx.iw + ctx.ih * ctx.ih;
        float diag = sqrtf((float)d2);
        float neg_half = diag * -0.5f;
        int ih_half = (int)neg_half;
        ctx.my = 2 - ih_half - ctx.ih / 2;
        ctx.H = ctx.ih + 2 * ctx.my;
        ctx.mx = 2 - ih_half - ctx.iw / 2;
        ctx.W = ctx.iw + 2 * ctx.mx;
    }

    try {
        const size_t canvas_px = (size_t)ctx.W * (size_t)ctx.H;
        ctx.A.assign(canvas_px * 4u, 0.0f);
        ctx.B.assign(canvas_px * 4u, 0.0f);
        ctx.wsum.assign(canvas_px, 0.0f);
        ctx.alpha.assign(canvas_px, 0.0f);
        ctx.comp.assign(canvas_px * 4u, 0.0f);
        ctx.mask.assign(canvas_px, 0);

        PF_Rect *area = (PF_Rect *)&output->extent_hint;
        const A_long progress_final = output->extent_hint.bottom - output->extent_hint.top;

        // 1. read the input layer into the canvas (PF_iterate over output extent).
        ERR(iterate_pass(in_data, out_data, bitdepth, true, input, area, &ctx, output,
                         progress_final));

        // 2. optional noise source.
        if (!err && p.mode == OLMDB_MODE_LAYER) {
            PF_ParamDef noise_param;
            AEFX_CLR_STRUCT(noise_param);
            ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_NOISE_LAYER, in_data->current_time,
                                  in_data->time_step, in_data->time_scale, &noise_param));
            if (!err) {
                const PF_LayerDef &ld = noise_param.u.ld;
                bool dims_ok = ld.data != nullptr
                    ? ((float)ld.height == ceilf((float)in_data->height * p.scale) &&
                       (float)ld.width == ceilf((float)in_data->width * p.scale))
                    : false;
                if (dims_ok) {
                    ctx.nzin.assign(canvas_px, 0.0f);
                    ctx.nzrot.assign(canvas_px, 0.0f);
                    for (int y = 0; y < ctx.ih; ++y) {
                        for (int x = 0; x < ctx.iw; ++x) {
                            int nx = x + input->origin_x - ld.origin_x;
                            int ny = y + input->origin_y - ld.origin_y;
                            float *dst = &ctx.nzin[ctx.pix(x + ctx.mx, y + ctx.my)];
                            if (nx < 0 || nx >= ld.width || ny < 0 || ny >= ld.height) {
                                *dst = 0.0f;
                            } else if (fmt == PF_PixelFormat_ARGB32) {
                                const PF_Pixel8 *sp =
                                    (const PF_Pixel8 *)((const char *)ld.data + (size_t)ny * (size_t)ld.rowbytes) + nx;
                                float a = (float)sp->alpha / 255.0f;
                                *dst = ((float)sp->green / 255.0f) * a * 0.587f +
                                       ((float)sp->red / 255.0f) * a * 0.299f +
                                       ((float)sp->blue / 255.0f) * a * 0.114f;
                            } else if (fmt == PF_PixelFormat_ARGB64) {
                                const PF_Pixel16 *sp =
                                    (const PF_Pixel16 *)((const char *)ld.data + (size_t)ny * (size_t)ld.rowbytes) + nx;
                                float a = (float)sp->alpha / 32768.0f;
                                *dst = ((float)sp->green / 32768.0f) * a * 0.587f +
                                       ((float)sp->red / 32768.0f) * a * 0.299f +
                                       ((float)sp->blue / 32768.0f) * a * 0.114f;
                            } else {
                                const PF_PixelFloat *sp =
                                    (const PF_PixelFloat *)((const char *)ld.data + (size_t)ny * (size_t)ld.rowbytes) + nx;
                                *dst = sp->green * sp->alpha * 0.587f +
                                       sp->red * sp->alpha * 0.299f +
                                       sp->blue * sp->alpha * 0.114f;
                            }
                        }
                    }
                    ctx.have_noise_plane = true;
                }
                ERR2(PF_CHECKIN_PARAM(in_data, &noise_param));
            }
        }

        // 3. generated noise field (built before the rotate in the original;
        //    rotation-independent, sampled in canvas space later).
        if (!err && p.mode == OLMDB_MODE_GENERATED)
            build_noise_field(ctx.nf, ctx.W, ctx.H, p.thickness, p.offset, p.seed);

        // 4. rotate canvas by +angle; rotate noise plane as well.
        if (!err) {
            rotate_rgba(ctx.A.data(), ctx.B.data(), ctx.W, ctx.H, (float)p.angle_rad);
            if (ctx.have_noise_plane)
                rotate_plane(ctx.nzin.data(), ctx.nzrot.data(), ctx.W, ctx.H, (float)p.angle_rad);
            memcpy(ctx.A.data(), ctx.B.data(), canvas_px * 4u * sizeof(float));
        }

        // 5. component analysis when Sharp Tail / Size Variation are active.
        if (!err) {
            if (p.back_tail >= 0.0001f || p.front_tail >= 0.0001f || p.size_var >= 0.0001f) {
                for (size_t i = 0; i < canvas_px; ++i)
                    ctx.mask[i] = (ctx.A[i * 4u + 3u] > 0.0f) ? 1u : 0u;
                component_pass(ctx);
            } else {
                for (size_t i = 0; i < canvas_px; ++i) {
                    ctx.comp[i * 4u + 0u] = 1.0f;
                    ctx.comp[i * 4u + 1u] = 1.0f;
                    ctx.comp[i * 4u + 2u] = 1.0f;
                    ctx.comp[i * 4u + 3u] = 1.0f;
                }
                ctx.maxlevel = 1.0f;
            }
        }

        // 6. LUTs.
        if (!err) {
            if (p.front_blur > 0) build_lut(ctx.lut_front_blur, p.front_blur);
            if (p.back_blur > 0) build_lut(ctx.lut_back_blur, p.back_blur);
            if (p.front_fade > 0) build_lut(ctx.lut_front_fade, p.front_fade);
            if (p.back_fade > 0) build_lut(ctx.lut_back_fade, p.back_fade);
        }

        // 7. blur core. The Windows kernel splits the canvas into min(H,32) row chunks of
        //    H/min(H,32) rows (integer division) and blurs only those rows, so rows past
        //    that are never blurred (visible on small layers with H > 32 and H % 32 != 0).
        if (!err && ctx.H > 0) {
            const int nchunks = ctx.H < 32 ? ctx.H : 32;
            const int chunk = ctx.H / nchunks;
            for (int y = 0; y < nchunks * chunk; ++y)
                blur_row(ctx, y);
        }

        // 8. normalize RGB by weight, clear source canvas.
        if (!err) {
            for (size_t i = 0; i < canvas_px; ++i) {
                float w = ctx.wsum[i];
                float *b = &ctx.B[i * 4u];
                if (w > 0.0f) {
                    b[0] /= w;
                    b[1] /= w;
                    b[2] /= w;
                }
                float *a = &ctx.A[i * 4u];
                a[0] = a[1] = a[2] = a[3] = 0.0f;
            }
        }

        // 9. rotate back and copy into the source canvas slot used by the writer.
        if (!err)
            rotate_rgba(ctx.B.data(), ctx.A.data(), ctx.W, ctx.H, -(float)p.angle_rad);

        // 10. write the output layer (PF_iterate over output extent).
        if (!err)
            ERR(iterate_pass(in_data, out_data, bitdepth, false, input, area, &ctx, output,
                             progress_final));
    } catch (const std::bad_alloc &) {
        err = PF_Err_OUT_OF_MEMORY;
    } catch (...) {
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return err;
}

//========================================================= simple callbacks ===
PF_Err About(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[], PF_LayerDef *output) {
    (void)in_data; (void)params; (void)output;
    PF_SPRINTF(out_data->return_msg, "%s v%d.%d.%d\r%s", OLMDB_NAME, OLMDB_MAJOR_VERSION,
               OLMDB_MINOR_VERSION, OLMDB_BUG_VERSION, OLMDB_DESCRIPTION);
    return PF_Err_NONE;
}

AEGP_PluginID g_aegp_id = 0;

// Windows: my_version 559104, out_flags 0x06000040 (USE_OUTPUT_EXTENT |
// DEEP_COLOR_AWARE | SEND_UPDATE_PARAMS_UI), out_flags2 0x08001408
// (PARAM_GROUP_START_COLLAPSED | SMART_RENDER | FLOAT_COLOR_AWARE | THREADED).
PF_Err GlobalSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[], PF_LayerDef *output) {
    (void)params; (void)output;
    out_data->my_version = PF_VERSION(OLMDB_MAJOR_VERSION, OLMDB_MINOR_VERSION, OLMDB_BUG_VERSION,
                                      OLMDB_STAGE_VERSION, OLMDB_BUILD_VERSION);
    out_data->out_flags = PF_OutFlag_USE_OUTPUT_EXTENT | PF_OutFlag_DEEP_COLOR_AWARE |
                          PF_OutFlag_SEND_UPDATE_PARAMS_UI;
    out_data->out_flags2 = PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG |
                           PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING;

    // Original registers with AEGP here ("AEGP Utility Suite" v7 = UtilitySuite3).
    if (in_data->pica_basicP) {
        AEGP_SuiteHandler suites(in_data->pica_basicP);
        suites.UtilitySuite3()->AEGP_RegisterWithAEGP(
            (AEGP_GlobalRefcon)0, OLMDB_NAME, &g_aegp_id);
    }
    return PF_Err_NONE;
}

// Params exactly as decoded from sub_180007310 (order, names, ranges, defaults).
PF_Err ParamsSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[], PF_LayerDef *output) {
    (void)params; (void)output;
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Angle", 0, OLMDB_ANGLE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDER("Brightness Gain", 0, 10, 0, 2, AEFX_DEFAULT_CURVE_TOLERANCE, 1.0, 2,
                        PF_ValueDisplayFlag_NONE, false, OLMDB_BRIGHTNESS);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FIXED("Size Variation", 0, 100, 0, 100, 0, 1, PF_ValueDisplayFlag_PERCENT, 0, OLMDB_SIZE_VAR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Front Blur Parameters", OLMDB_FRONT_TOPIC);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Blur Strength", 0, 4000, 0, 4000, 0, OLMDB_FRONT_BLUR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Alpha Fade", 0, 100, 0, 100, 0, OLMDB_FRONT_FADE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FIXED("Sharp Tail", 0, 100, 0, 100, 0, 1, PF_ValueDisplayFlag_PERCENT, 0, OLMDB_FRONT_TAIL);

    AEFX_CLR_STRUCT(def);
    PF_STRNNCPY(def.PF_DEF_NAME, "Sharp Tail", sizeof(def.PF_DEF_NAME));  // binary names group ends
    PF_END_TOPIC(OLMDB_FRONT_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Back Blur Parameters", OLMDB_BACK_TOPIC);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Blur Strength", 0, 4000, 0, 4000, 0, OLMDB_BACK_BLUR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Alpha Fade", 0, 100, 0, 100, 0, OLMDB_BACK_FADE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FIXED("Sharp Tail", 0, 100, 0, 100, 0, 1, PF_ValueDisplayFlag_PERCENT, 0, OLMDB_BACK_TAIL);

    AEFX_CLR_STRUCT(def);
    PF_STRNNCPY(def.PF_DEF_NAME, "Sharp Tail", sizeof(def.PF_DEF_NAME));  // binary names group ends
    PF_END_TOPIC(OLMDB_BACK_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Noise Parameters", OLMDB_NOISE_TOPIC);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FIXED("Noise Variation", 0, 100, 0, 100, 0, 1, PF_ValueDisplayFlag_PERCENT, 0, OLMDB_NOISE_VAR);

    // Binary ships num_choices = 2 with a 3-item string (bug-compatible).
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Noise Type", 2, 1, "Smooth | Block | Layer", OLMDB_NOISE_TYPE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Noise Layer", 0, OLMDB_NOISE_LAYER);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Seed", 1, 1000, 1, 1000, 1, OLMDB_SEED);

    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Offset", 0, OLMDB_OFFSET);

    // Binary stores curve_tolerance = 10.0 for Thickness (harmless for a
    // non-audio effect; kept for binary fidelity).
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDER("Thickness", 1, 100, 1, 100, 10.0, 10.0, 2, PF_ValueDisplayFlag_NONE, false,
                        OLMDB_THICKNESS);

    AEFX_CLR_STRUCT(def);
    PF_STRNNCPY(def.PF_DEF_NAME, "Thickness", sizeof(def.PF_DEF_NAME));  // binary names group ends
    PF_END_TOPIC(OLMDB_NOISE_END);

    out_data->num_params = OLMDB_NUM_PARAMS;
    return err;
}

//=============================================== update params UI (AEGP) =====
// Original (sub_180007EA0/sub_1800081C0): at UPDATE_PARAMS_UI, fetch effect
// layer + current time, get effect ref, then hide params via
// AEGP_SetDynamicStreamFlag(AEGP_DynStreamFlag_HIDDEN):
//   Layer mode   -> hide Seed/Offset/Thickness
//   Smooth/Block -> hide Noise Layer
PF_Err SetStreamHidden(AEGP_SuiteHandler &suites, AEGP_EffectRefH effectH, PF_ParamIndex index,
                       PF_Boolean hide) {
    AEGP_StreamRefH streamH = nullptr;
    PF_Err err = suites.StreamSuite7()->AEGP_GetNewEffectStreamByIndex(g_aegp_id, effectH, index,
                                                                       &streamH);
    if (!err) {
        AEGP_DynStreamFlags flags = 0;
        err = suites.DynamicStreamSuite3()->AEGP_GetDynamicStreamFlags(streamH, &flags);
        if (!err)
            err = suites.DynamicStreamSuite3()->AEGP_SetDynamicStreamFlag(
                streamH, AEGP_DynStreamFlag_HIDDEN, false, hide);
        suites.StreamSuite7()->AEGP_DisposeStream(streamH);
    }
    return err;
}

PF_Err UpdateParamsUI(PF_InData *in_data, PF_OutData *out_data) {
    (void)out_data;
    PF_Err err = PF_Err_NONE;
    if (!in_data->pica_basicP || g_aegp_id == 0)
        return err;

    PF_ParamDef cur;
    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_NOISE_TYPE, in_data->current_time, in_data->time_step,
                          in_data->time_scale, &cur));
    int noise_type = cur.u.pd.value;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));
    if (err) return err;

    AEGP_SuiteHandler suites(in_data->pica_basicP);
    AEGP_LayerH layerH = nullptr;
    ERR(suites.PFInterfaceSuite1()->AEGP_GetEffectLayer(in_data->effect_ref, &layerH));
    if (err || !layerH) return err;
    {
        A_Time time;
        AEFX_CLR_STRUCT(time);
        ERR(suites.LayerSuite5()->AEGP_GetLayerCurrentTime(layerH, AEGP_LTimeMode_LayerTime, &time));
    }
    AEGP_EffectRefH effectH = nullptr;
    ERR(suites.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(g_aegp_id, in_data->effect_ref,
                                                               &effectH));
    if (err || !effectH) return err;

    const PF_Boolean is_layer = (noise_type == OLMDB_POPUP_LAYER);
    ERR(SetStreamHidden(suites, effectH, OLMDB_NOISE_LAYER, !is_layer));
    ERR(SetStreamHidden(suites, effectH, OLMDB_SEED, is_layer));
    ERR(SetStreamHidden(suites, effectH, OLMDB_OFFSET, is_layer));
    ERR(SetStreamHidden(suites, effectH, OLMDB_THICKNESS, is_layer));
    suites.EffectSuite5()->AEGP_DisposeEffect(effectH);
    return err;
}

//============================================================== pre-render =====
PF_Err PreRender(PF_InData *in_data, PF_OutData *out_data, PF_PreRenderExtra *extra) {
    (void)out_data;
    PF_Err err = PF_Err_NONE;
    PF_CheckoutResult in_result;
    PF_RenderRequest req = extra->input->output_request;

    // Binary's PF_Cmd_SMART_PRE_RENDER: checkout the input layer, copy rects.
    // Parameters are checked out during SMART_RENDER, not here.
    ERR(extra->cb->checkout_layer(in_data->effect_ref, OLMDB_INPUT, 0, &req, in_data->current_time,
                                  in_data->time_step, in_data->time_scale, &in_result));
    if (!err) {
        UnionLRect(&in_result.result_rect, &extra->output->result_rect);
        UnionLRect(&in_result.max_result_rect, &extra->output->max_result_rect);
    }
    return err;
}

//============================================================ smart render =====
PF_Err SmartRender(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra) {
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    PF_EffectWorld *input = nullptr, *output = nullptr;

    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, OLMDB_INPUT, &input));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &output));

    OLMDBParams p;
    if (!err)
        ERR(checkout_params(in_data, &p));

    // The Windows kernel's first action is utils->copy(input, output) (the
    // unidentified 5-arg call at sub_180004A20/_3C90/_57B0 +0). It is what
    // makes the all-zero-parameter early-out harmless: with default settings
    // the layer passes through unchanged instead of being left unwritten.
    if (!err && input && output)
        ERR(in_data->utils->copy(in_data->effect_ref, input, output, nullptr, nullptr));

    if (!err && input && output) {
        AEFX_SuiteScoper<PF_WorldSuite2> ws(in_data, kPFWorldSuite, kPFWorldSuiteVersion2, out_data);
        PF_PixelFormat fmt = PF_PixelFormat_INVALID;
        ERR(ws->PF_GetPixelFormat(input, &fmt));
        const int bitdepth = extra->input->bitdepth;

        if (!err && fmt != PF_PixelFormat_ARGB32 && fmt != PF_PixelFormat_ARGB64 &&
            fmt != PF_PixelFormat_ARGB128)
            err = PF_Err_BAD_CALLBACK_PARAM;
        if (!err && bitdepth != 8 && bitdepth != 16 && bitdepth != 32)
            err = PF_Err_BAD_CALLBACK_PARAM;

        if (!err) {
            // Downsample scaling (only x, as original) then all-zero early out.
            p.scale = (float)in_data->downsample_x.num / (float)in_data->downsample_x.den;
            p.front_blur = (int)((float)p.front_blur * p.scale);
            p.front_fade = (int)((float)p.front_fade * p.scale);
            p.back_blur = (int)((float)p.back_blur * p.scale);
            p.back_fade = (int)((float)p.back_fade * p.scale);
            p.thickness = p.scale * p.thickness;
            if (!p.front_blur && !p.back_blur && !p.front_fade && !p.back_fade) {
                // Original returns without touching the output world here.
                ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, OLMDB_INPUT));
                return PF_Err_NONE;
            }
            err = render_format(in_data, out_data, input, output, fmt, bitdepth, p);
        }
    }

    ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, OLMDB_INPUT));
    return err;
}

}  // namespace

//=================================================================== entry =====
PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[],
                  PF_LayerDef *output, void *extra) {
    PF_Err err = PF_Err_NONE;
    try {
        switch (cmd) {
        case PF_Cmd_ABOUT:
            err = About(in_data, out_data, params, output);
            break;
        case PF_Cmd_GLOBAL_SETUP:
            err = GlobalSetup(in_data, out_data, params, output);
            break;
        case PF_Cmd_PARAMS_SETUP:
            err = ParamsSetup(in_data, out_data, params, output);
            break;
        case PF_Cmd_RENDER:
            break;  // smart-render effect; binary's RENDER handler is a no-op
        case PF_Cmd_UPDATE_PARAMS_UI:
            err = UpdateParamsUI(in_data, out_data);
            break;
        case PF_Cmd_SMART_PRE_RENDER:
            err = PreRender(in_data, out_data, (PF_PreRenderExtra *)extra);
            break;
        case PF_Cmd_SMART_RENDER:
            err = SmartRender(in_data, out_data, (PF_SmartRenderExtra *)extra);
            break;
        default:
            break;
        }
    } catch (PF_Err &e) {
        err = e;
    } catch (...) {
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return err;
}

// OLM Anti Anti Alias 1.1.6 - macOS port.
//
// Verified against OLMAntiAntiAlias.aex (IDA; see %TEMP%\opencode\specs\OLMAntiAntiAlias.md):
//   PiPL/params/dispatch, PARAMS_SETUP, colour scan, pixel-classification passes 1-3, pair fit
//   (0x18000A610 / A9C0 / AC80), ring search (0x180003720), pre-render rect union (0x18000BEF0).
// Deviations (documented): SEND_UPDATE_PARAMS_UI added to out_flags; legacy PF_Cmd_RENDER is a
// no-op (SMART_RENDER is the only render path under AE 2026); AEGP render uses RenderSuite5.

#include "OLMAntiAntiAlias.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

AEGP_PluginID g_aegp_id = 0;

const float kScanTol = 0.0002f;   // sub_180005230: |d| <= 0.0002 on every channel
const float kBigDist = 99999.9f;  // initial "best" value in the pick loops

// Binary clamp used for the unmixed colours: v >= 0 ? min(1, v) : 0.
inline float Clamp01(float v) {
    if (v >= 0.0f) return v < 1.0f ? v : 1.0f;
    return 0.0f;
}

inline float Dist(const AAAColor &a, const AAAColor &b) {
    const float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
    const float s = dr * dr + dg * dg + db * db;
    return std::sqrt(s) + 0.0001f;  // sub_18000A0E0
}

inline bool NearlyEqual(const AAAColor &a, const AAAColor &b) {
    return std::fabs(a.r - b.r) <= kScanTol && std::fabs(a.g - b.g) <= kScanTol &&
           std::fabs(a.b - b.b) <= kScanTol && std::fabs(a.a - b.a) <= kScanTol;
}

// ---------------------------------------------------------------- palette ---
// OLMAAAContext colour set plus the pairwise tables from sub_180009500:
//   D[i + N*j] = col[j] - col[i]   (RGB, alpha slot 0)   ctx+216
//   D2[i + N*j] = |col[j] - col[i]|^2                    ctx+264
struct Palette {
    int n = 0;
    std::vector<AAAColor> col;
    std::vector<AAAColor> D;
    std::vector<float> D2;

    void BuildPairs() {
        D.assign((size_t)n * n, AAAColor{0, 0, 0, 0});
        D2.assign((size_t)n * n, 0.0f);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const AAAColor &cx = col[x], &cy = col[y];
                AAAColor d{0.0f, cy.r - cx.r, cy.g - cx.g, cy.b - cx.b};
                const size_t idx = (size_t)x + (size_t)n * y;
                D[idx] = d;
                D2[idx] = d.r * d.r + d.g * d.g + d.b * d.b;
            }
        }
    }
};

// sub_18000A610: cost of explaining pixel P as a blend of C1 (= palette i) and C2 (= palette j).
// Dv = C2 - C1 (direction), dd = |Dv|^2. Returns cost; d1/d2 = |P-C1|+eps and |P-C2|+eps.
float PairCost(const AAAColor &P, const AAAColor &C1, const AAAColor &C2, const AAAColor &Dv,
               float dd, float &d1o, float &d2o) {
    d1o = Dist(P, C1);
    d2o = Dist(P, C2);
    const float tt = ((P.g - C1.g) * Dv.g + (P.r - C1.r) * Dv.r + (P.b - C1.b) * Dv.b) / dd;
    // Identical palette colours give 0/0 = NaN here. The binary's cost is then never accepted by the
    // "best > cost" update (comiss + jbe), so such pairs cannot be picked. Reproduced verbatim.
    if (std::isnan(tt)) return std::nanf("");
    if (tt < 1.0f) {
        if (tt > 0.0f) {
            AAAColor A, B;
            A.r = Clamp01(P.r - Dv.r * tt);
            A.g = Clamp01(P.g - Dv.g * tt);
            A.b = Clamp01(P.b - Dv.b * tt);
            const float s = 1.0f - tt;
            B.r = Clamp01((P.r - s * A.r) / tt);
            B.g = Clamp01((P.g - s * A.g) / tt);
            B.b = Clamp01((P.b - s * A.b) / tt);
            const float ar = A.r - C1.r, ag = A.g - C1.g, ab = A.b - C1.b;
            const float sa = std::sqrt(ar * ar + ag * ag + ab * ab) + 0.0001f;
            const float br = B.r - C2.r, bg = B.g - C2.g, bb = B.b - C2.b;
            const float sb = std::sqrt(br * br + bg * bg + bb * bb);
            return (sb + 0.0001f) + sa;
        }
        return d1o;
    }
    return d2o;
}

// sub_18000A9C0: best pair among candidate palette indices (cand, n >= 2). Pairs k<m in list order.
// Returns (first, second); first/second are -1 when no pair beats kBigDist.
void BestPair(const Palette &p, const AAAColor &P, const std::vector<int> &cand, int &first,
              int &second) {
    float best = kBigDist;
    first = -1;
    second = -1;
    const int n = (int)cand.size();
    for (int k = 0; k < n; ++k) {
        for (int m = k + 1; m < n; ++m) {
            const int i = cand[k], j = cand[m];
            const size_t idx = (size_t)i + (size_t)p.n * j;
            float d1, d2;
            const float cost = PairCost(P, p.col[i], p.col[j], p.D[idx], p.D2[idx], d1, d2);
            if (best > cost) {
                best = cost;
                if (d2 <= d1) {
                    first = j;
                    second = i;
                } else {
                    first = i;
                    second = j;
                }
            }
        }
    }
}

// sub_18000AC80: best partner for a single candidate i over the whole palette (j != i).
void SinglePair(const Palette &p, const AAAColor &P, int i, int &first, int &second) {
    float best = kBigDist;
    first = -1;
    second = -1;
    for (int j = 0; j < p.n; ++j) {
        if (j == i) continue;
        const size_t idx = (size_t)i + (size_t)p.n * j;
        float d1, d2;
        const float cost = PairCost(P, p.col[i], p.col[j], p.D[idx], p.D2[idx], d1, d2);
        if (best > cost) {
            best = cost;
            if (d2 <= d1) {
                first = j;
                second = i;
            } else {
                first = i;
                second = j;
            }
        }
    }
}

// sub_18000A150: nearest palette index by RGB squared distance (first minimum), -1 if none.
int Nearest(const Palette &p, const AAAColor &c) {
    float best = kBigDist;
    int idx = -1;
    for (int i = 0; i < p.n; ++i) {
        const AAAColor &e = p.col[i];
        const float dr = e.r - c.r, dg = e.g - c.g, db = e.b - c.b;
        const float s = dr * dr + dg * dg + db * db;
        if (best > s) {
            best = s;
            idx = i;
        }
    }
    return idx;
}

// sub_18000A330: nearest among a candidate index list (first minimum), -1 if none.
int NearestIn(const Palette &p, const AAAColor &c, const std::vector<int> &cand) {
    float best = kBigDist;
    int idx = -1;
    for (int i : cand) {
        const AAAColor &e = p.col[i];
        const float dr = e.r - c.r, dg = e.g - c.g, db = e.b - c.b;
        const float s = dr * dr + dg * dg + db * db;
        if (best > s) {
            best = s;
            idx = i;
        }
    }
    return idx;
}

// ----------------------------------------------------------- ring search ---
// sub_180003720: square of radius r around (x,y). Matched neighbours (A >= 0) flag their index;
// unmatched neighbours flag their nearest palette colour when it is within distance 1.0 of P.
// Returns the first palette index of the pair pick, or -1.
int RingPick(const Palette &p, const std::vector<int> &A, const std::vector<AAAColor> &inF,
             int W, int H, int x, int y, int r, const AAAColor &P) {
    std::vector<char> flag(p.n, 0);
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
            const size_t pos = (size_t)nx + (size_t)W * ny;
            const int a = A[pos];
            if (a >= 0) {
                flag[a] = 1;
            } else {
                const int k = Nearest(p, inF[pos]);
                if (k >= 0 && Dist(P, p.col[k]) < 1.0f) flag[k] = 1;
            }
        }
    }
    std::vector<int> cand;
    for (int i = 0; i < p.n; ++i)
        if (flag[i]) cand.push_back(i);
    if (cand.empty()) return -1;
    int first = -1, second = -1;
    if (cand.size() == 1)
        SinglePair(p, P, cand[0], first, second);
    else
        BestPair(p, P, cand, first, second);
    return first;
}

// ---------------------------------------------------------- pixel formats ---
struct Pix8 {
    using T = PF_Pixel8;
    static constexpr int kBpc = 8;
    static AAAColor read(const T &v) {
        return {(float)((double)v.alpha / 255.0), (float)((double)v.red / 255.0),
                (float)((double)v.green / 255.0), (float)((double)v.blue / 255.0)};
    }
    static void write(T &v, const AAAColor &c) {
        v.alpha = (A_u_char)(int)(float)(c.a * 255.0);
        v.red = (A_u_char)(int)(float)(c.r * 255.0);
        v.green = (A_u_char)(int)(float)(c.g * 255.0);
        v.blue = (A_u_char)(int)(float)(c.b * 255.0);
    }
    // pass-1 exact match: (u8)(int)((c + 1/510) * 255) against the stored byte
    static bool same(const T &v, const AAAColor &c) {
        return v.alpha == (A_u_char)(int)((float)(c.a + 0.0019607844f) * 255.0) &&
               v.red == (A_u_char)(int)((float)(c.r + 0.0019607844f) * 255.0) &&
               v.green == (A_u_char)(int)((float)(c.g + 0.0019607844f) * 255.0) &&
               v.blue == (A_u_char)(int)((float)(c.b + 0.0019607844f) * 255.0);
    }
};

struct Pix16 {
    using T = PF_Pixel16;
    static constexpr int kBpc = 16;
    static AAAColor read(const T &v) {
        return {(float)((double)v.alpha / 32768.0), (float)((double)v.red / 32768.0),
                (float)((double)v.green / 32768.0), (float)((double)v.blue / 32768.0)};
    }
    static void write(T &v, const AAAColor &c) {
        v.alpha = (A_u_short)(int)(float)(c.a * 32768.0);
        v.red = (A_u_short)(int)(float)(c.r * 32768.0);
        v.green = (A_u_short)(int)(float)(c.g * 32768.0);
        v.blue = (A_u_short)(int)(float)(c.b * 32768.0);
    }
    static bool same(const T &v, const AAAColor &c) {
        return v.alpha == (A_u_short)(int)((float)(c.a + 1.0f / 65536.0f) * 32768.0) &&
               v.red == (A_u_short)(int)((float)(c.r + 1.0f / 65536.0f) * 32768.0) &&
               v.green == (A_u_short)(int)((float)(c.g + 1.0f / 65536.0f) * 32768.0) &&
               v.blue == (A_u_short)(int)((float)(c.b + 1.0f / 65536.0f) * 32768.0);
    }
};

struct PixF {
    using T = PF_PixelFloat;
    static constexpr int kBpc = 32;
    static AAAColor read(const T &v) { return {v.alpha, v.red, v.green, v.blue}; }
    static void write(T &v, const AAAColor &c) {
        v.alpha = c.a;
        v.red = c.r;
        v.green = c.g;
        v.blue = c.b;
    }
    static bool same(const T &v, const AAAColor &c) {
        return std::fabs(v.red - c.r) <= 0.0001f && std::fabs(v.green - c.g) <= 0.0001f &&
               std::fabs(v.blue - c.b) <= 0.0001f && std::fabs(v.alpha - c.a) <= 0.0001f;
    }
};

// Full-format passes. Pass 1 covers the output extent; passes 2/3 cover the whole input canvas
// (sub_1800018E0 uses the input world dimensions).
template <class F>
void RunKernel(const Palette &p, const typename F::T *srcBase, int srcRowBytes, typename F::T *dstBase,
               int dstRowBytes, int W, int H, const PF_Rect &extent, std::vector<int> &A,
               std::vector<int> &B) {
    using T = typename F::T;
    auto rowS = [&](int y) { return (const T *)((const char *)srcBase + (size_t)y * srcRowBytes); };
    auto rowD = [&](int y) { return (T *)((char *)dstBase + (size_t)y * dstRowBytes); };

    // Pass 1 (sub_180002100 / 1E10 / 23C0): classify over the output extent.
    for (A_long y = extent.top; y < extent.bottom; ++y) {
        for (A_long x = extent.left; x < extent.right; ++x) {
            const T &s = rowS(y)[x];
            T &d = rowD(y)[x];
            const AAAColor sc = F::read(s);
            if (sc.a == 0.0f) {
                // alpha 0: copy, flag 0 (8-bit writes (0,R,G,B); float copies all four)
                d = s;
                continue;
            }
            int k = -1;
            for (int i = 0; i < p.n; ++i) {
                if (F::same(s, p.col[i])) {
                    k = i;
                    break;
                }
            }
            if (k >= 0) {
                AAAColor out = p.col[k];
                out.a = 1.0f;
                F::write(d, out);
                A[(size_t)x + (size_t)W * y] = k;
                B[(size_t)x + (size_t)W * y] = k;
            } else {
                F::write(d, AAAColor{1.0f, 0.0f, 0.0f, 0.0f});
            }
        }
    }

    // Pass 2 (4210 / 3F90 / 4470): unmatched opaque pixels take a ring-picked palette colour.
    std::vector<AAAColor> inF((size_t)W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) inF[(size_t)x + (size_t)W * y] = F::read(rowS(y)[x]);

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const size_t pos = (size_t)x + (size_t)W * y;
            const AAAColor &sc = inF[pos];
            const T &s = rowS(y)[x];
            if (sc.a == 0.0f) {
                rowD(y)[x] = s;
                continue;
            }
            if (A[pos] >= 0) continue;  // exact match: pass-1 output stands
            if (sc.a == 1.0f) {
                int pick = -1;
                for (int r = 1; r <= 2 && pick == -1; ++r)
                    pick = RingPick(p, A, inF, W, H, x, y, r, sc);
                if (pick != -1) {
                    B[pos] = pick;
                    AAAColor out = p.col[pick];
                    out.a = 1.0f;
                    F::write(rowD(y)[x], out);
                } else {
                    rowD(y)[x] = s;
                }
            } else {
                rowD(y)[x] = s;  // partial alpha: keep own colour
            }
        }
    }

    // Pass 3 (2AB0 / 2620 / 2F20): partial-alpha pixels blend toward nearby matched colours.
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const size_t pos = (size_t)x + (size_t)W * y;
            const AAAColor &sc = inF[pos];
            if (sc.a == 1.0f) continue;
            if (B[pos] >= 0) continue;
            T &d = rowD(y)[x];
            const T &s = rowS(y)[x];
            if (sc.a <= 0.5f) {
                F::write(d, AAAColor{0.0f, 1.0f, 1.0f, 1.0f});
                continue;
            }
            std::vector<char> flag(p.n, 0);
            bool found = false;
            for (int r = 1; r <= 4 && !found; ++r) {
                for (int dy = -r; dy <= r; ++dy) {
                    for (int dx = -r; dx <= r; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                        const int bi = B[(size_t)nx + (size_t)W * ny];
                        if (bi >= 0) {
                            flag[bi] = 1;
                            found = true;
                        }
                    }
                }
            }
            if (!found) {
                d = s;
                continue;
            }
            std::vector<int> cand;
            for (int i = 0; i < p.n; ++i)
                if (flag[i]) cand.push_back(i);
            const int idx = NearestIn(p, sc, cand);
            if (idx < 0) {
                d = s;
            } else {
                AAAColor out = p.col[idx];
                out.a = 1.0f;
                F::write(d, out);
            }
        }
    }
}

// --------------------------------------------------------- auto-colour scan ---
// sub_180005230 (float world; 8/16 variants use the same rule on their own samples).
// Appends the colour of every opaque pixel whose 3x3 neighbourhood is uniform, unless it is
// already in `existing` (count fixed) or in `out`. Scan stops when out.size() reaches cap.
template <class F>
void ScanWorld(const PF_EffectWorld *world, const std::vector<AAAColor> &existing,
               std::vector<AAAColor> &out, size_t cap) {
    using T = typename F::T;
    const int W = world->width, H = world->height;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W && out.size() < cap; ++x) {
            const T *row = (const T *)((const char *)world->data + (size_t)y * world->rowbytes);
            const AAAColor c = F::read(row[x]);
            if (c.a != 1.0f) continue;
            bool uniform = x > 0 && y > 0 && x < W - 1 && y < H - 1;
            if (uniform) {
                for (int dy = -1; dy <= 1 && uniform; ++dy) {
                    for (int dx = -1; dx <= 1 && uniform; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const T *nr = (const T *)((const char *)world->data +
                                                  (size_t)(y + dy) * world->rowbytes);
                        const AAAColor n = F::read(nr[x + dx]);
                        if (!(n.r == c.r && n.g == c.g && n.b == c.b && n.a == c.a)) uniform = false;
                    }
                }
            }
            if (!uniform) continue;
            bool seen = false;
            for (const AAAColor &e : existing)
                if (NearlyEqual(c, e)) {
                    seen = true;
                    break;
                }
            if (!seen)
                for (const AAAColor &e : out)
                    if (NearlyEqual(c, e)) {
                        seen = true;
                        break;
                    }
            if (!seen) out.push_back(c);
        }
        if (out.size() >= cap) break;
    }
}

// Dispatch the scan on the world's pixel format (argb = 8, ae16 = 16, ae32 = 32 bpc).
void ScanByFormat(const PF_EffectWorld *world, int bpc, const std::vector<AAAColor> &existing,
                  std::vector<AAAColor> &out, size_t cap) {
    switch (bpc) {
        case 16:
            ScanWorld<Pix16>(world, existing, out, cap);
            break;
        case 32:
            ScanWorld<PixF>(world, existing, out, cap);
            break;
        default:
            ScanWorld<Pix8>(world, existing, out, cap);
            break;
    }
}

// ----------------------------------------------------------- param helpers ---
PF_Err CheckoutSlider(PF_InData *in, PF_ParamIndex idx, A_long &value) {
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    PF_Err err = PF_CHECKOUT_PARAM(in, idx, in->current_time, in->time_step, in->time_scale, &def);
    if (!err) value = (A_long)def.u.sd.value;
    const PF_Err ce = PF_CHECKIN_PARAM(in, &def);
    return err ? err : ce;
}

PF_Err CheckoutBool(PF_InData *in, PF_ParamIndex idx, bool &value) {
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    PF_Err err = PF_CHECKOUT_PARAM(in, idx, in->current_time, in->time_step, in->time_scale, &def);
    if (!err) value = def.u.bd.value != 0;
    const PF_Err ce = PF_CHECKIN_PARAM(in, &def);
    return err ? err : ce;
}

PF_Err CheckoutColor(PF_InData *in, PF_OutData *out, PF_ParamIndex idx, AAAColor &c) {
    AEFX_SuiteScoper<PF_ColorParamSuite1> color_suite(in, kPFColorParamSuite,
                                                      kPFColorParamSuiteVersion1, out);
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    PF_Err err = PF_CHECKOUT_PARAM(in, idx, in->current_time, in->time_step, in->time_scale, &def);
    if (!err) {
        PF_PixelFloat px{};
        err = color_suite->PF_GetFloatingPointColorFromColorDef(in->effect_ref, &def, &px);
        c = AAAColor{px.alpha, px.red, px.green, px.blue};
    }
    const PF_Err ce = PF_CHECKIN_PARAM(in, &def);
    return err ? err : ce;
}

// Reads Number of Colors and the first `count` colours (params 7..).
PF_Err ReadColors(PF_InData *in, PF_OutData *out, A_long &count, std::vector<AAAColor> &cols) {
    PF_Err err = CheckoutSlider(in, OLMAAA_NUM_COLORS, count);
    if (err) return err;
    if (count < 0) count = 0;
    if (count > OLMAAA_COLOR_COUNT) count = OLMAAA_COLOR_COUNT;
    cols.clear();
    for (A_long i = 0; i < count; ++i) {
        AAAColor c{};
        err = CheckoutColor(in, out, OLMAAA_COLOR_FIRST + i, c);
        if (err) return err;
        cols.push_back(c);
    }
    return PF_Err_NONE;
}

// sub_18000BEF0: an empty destination takes the whole source; otherwise min/max union with a
// non-empty source only.
void UnionRect(const PF_Rect &src, PF_Rect &dst) {
    if (dst.left >= dst.right || dst.top >= dst.bottom) {
        dst = src;
        return;
    }
    if (src.left < src.right && src.top < src.bottom) {
        if (dst.left > src.left) dst.left = src.left;
        if (dst.top > src.top) dst.top = src.top;
        if (dst.right < src.right) dst.right = src.right;
        if (dst.bottom < src.bottom) dst.bottom = src.bottom;
    }
}

// --------------------------------------------------------------- render ---
PF_Err RenderPixels(PF_InData *in, PF_OutData *out, PF_SmartRenderExtra *extra) {
    PF_EffectWorld *inWorld = nullptr, *outWorld = nullptr;
    PF_Err err = extra->cb->checkout_layer_pixels(in->effect_ref, OLMAAA_INPUT, &inWorld);
    if (err) return err;
    err = extra->cb->checkout_output(in->effect_ref, &outWorld);
    if (err) return err;
    const int bpc = extra->input->bitdepth;

    A_long count = 0;
    bool use_auto = false;
    std::vector<AAAColor> cols;
    err = ReadColors(in, out, count, cols);
    if (!err) err = CheckoutBool(in, OLMAAA_USE_AUTO, use_auto);
    if (err) return err;

    // copy input -> output first (utils->copy in sub_1800014C0 / 1300 / 6360)
    err = in->utils->copy(in->effect_ref, inWorld, outWorld, nullptr, nullptr);
    if (err) return err;

    Palette p;
    p.col = cols;
    for (AAAColor &c : p.col) c.a = 1.0f;  // sub_180009860

    if (use_auto) {
        std::vector<AAAColor> none;
        std::vector<AAAColor> found;
        ScanByFormat(inWorld, bpc, none, found, 9999);
        // scanned colours are appended to the palette; the scan keeps alpha = 1
        for (const AAAColor &c : found) p.col.push_back(c);
    }
    p.n = (int)p.col.size();

    // pass-through: no colours, or one colour that is black
    if (p.n == 0 || (p.n == 1 && p.col[0].r == 0.0f && p.col[0].g == 0.0f && p.col[0].b == 0.0f))
        return PF_Err_NONE;

    const int W = inWorld->width, H = inWorld->height;
    p.BuildPairs();
    std::vector<int> A((size_t)W * H, -1), B((size_t)W * H, -1);
    const PF_Rect extent = outWorld->extent_hint;

    switch (bpc) {
        case 16:
            RunKernel<Pix16>(p, (const PF_Pixel16 *)inWorld->data, inWorld->rowbytes,
                             (PF_Pixel16 *)outWorld->data, outWorld->rowbytes, W, H, extent, A, B);
            break;
        case 32:
            RunKernel<PixF>(p, (const PF_PixelFloat *)inWorld->data, inWorld->rowbytes,
                            (PF_PixelFloat *)outWorld->data, outWorld->rowbytes, W, H, extent, A, B);
            break;
        default:
            RunKernel<Pix8>(p, (const PF_Pixel8 *)inWorld->data, inWorld->rowbytes,
                            (PF_Pixel8 *)outWorld->data, outWorld->rowbytes, W, H, extent, A, B);
            break;
    }
    return PF_Err_NONE;
}

PF_Err PreRender(PF_InData *in, PF_OutData *out, PF_PreRenderExtra *extra) {
    PF_RenderRequest req = extra->input->output_request;
    PF_CheckoutResult res;
    AEFX_CLR_STRUCT(res);
    PF_Err err = extra->cb->checkout_layer(in->effect_ref, OLMAAA_INPUT, 0, &req, in->current_time,
                                           in->time_step, in->time_scale, &res);
    if (err) return err;
    // sub_18000BEF0: union the checkout rects into the output rects.
    UnionRect(res.result_rect, extra->output->result_rect);
    UnionRect(res.max_result_rect, extra->output->max_result_rect);
    (void)out;
    return PF_Err_NONE;
}

// ------------------------------------------------------------ AEGP helpers ---
// Stream visibility: AEGP_SetDynamicStreamFlag(HIDDEN, undoable=false, hidden) on colour i.
PF_Err SetColorStreamHidden(AEGP_SuiteHandler &suites, AEGP_EffectRefH effectH, PF_ParamIndex pos,
                            bool hidden) {
    AEGP_StreamRefH streamH = nullptr;
    PF_Err err = suites.StreamSuite7()->AEGP_GetNewEffectStreamByIndex(g_aegp_id, effectH, pos,
                                                                       &streamH);
    if (!err && streamH) {
        // sub_1800066F0 reads the current dynamic flags before setting HIDDEN (result unused).
        AEGP_DynStreamFlags current = 0;
        suites.DynamicStreamSuite3()->AEGP_GetDynamicStreamFlags(streamH, &current);
        err = suites.DynamicStreamSuite3()->AEGP_SetDynamicStreamFlag(
            streamH, AEGP_DynStreamFlag_HIDDEN, false, hidden ? TRUE : FALSE);
        suites.StreamSuite7()->AEGP_DisposeStream(streamH);
    }
    return err;
}

// Writes one colour (alpha, r, g, b) to the colour stream at position pos.
PF_Err SetColorStream(AEGP_SuiteHandler &suites, AEGP_EffectRefH effectH, PF_ParamIndex pos,
                      const AAAColor &c) {
    AEGP_StreamRefH streamH = nullptr;
    PF_Err err = suites.StreamSuite7()->AEGP_GetNewEffectStreamByIndex(g_aegp_id, effectH, pos,
                                                                       &streamH);
    if (!err && streamH) {
        AEGP_StreamValue2 value;
        AEFX_CLR_STRUCT(value);
        value.streamH = streamH;
        value.val.color.alphaF = c.a;
        value.val.color.redF = c.r;
        value.val.color.greenF = c.g;
        value.val.color.blueF = c.b;
        err = suites.StreamSuite7()->AEGP_SetStreamValue(g_aegp_id, streamH, &value);
        suites.StreamSuite7()->AEGP_DisposeStream(streamH);
    }
    return err;
}

// Writes the Number of Colors slider value.
PF_Err SetCountStream(AEGP_SuiteHandler &suites, AEGP_EffectRefH effectH, A_long count) {
    AEGP_StreamRefH streamH = nullptr;
    PF_Err err = suites.StreamSuite7()->AEGP_GetNewEffectStreamByIndex(
        g_aegp_id, effectH, OLMAAA_NUM_COLORS, &streamH);
    if (!err && streamH) {
        AEGP_StreamValue2 value;
        AEFX_CLR_STRUCT(value);
        value.streamH = streamH;
        value.val.one_d = (AEGP_OneDVal)count;
        err = suites.StreamSuite7()->AEGP_SetStreamValue(g_aegp_id, streamH, &value);
        suites.StreamSuite7()->AEGP_DisposeStream(streamH);
    }
    return err;
}

// sub_180005B00: every colour stream 7..106 is set to transparent black (0,0,0,0).
PF_Err ClearAllColours(PF_InData *in, AEGP_SuiteHandler &suites, AEGP_EffectRefH effectH) {
    (void)in;
    PF_Err err = PF_Err_NONE;
    for (int i = 0; i < OLMAAA_COLOR_COUNT && !err; ++i)
        err = SetColorStream(suites, effectH, OLMAAA_COLOR_FIRST + i, AAAColor{0, 0, 0, 0});
    return err;
}

// Effect ref + render options + frame receipt helpers for the upstream render (scan source).
// The frame receipt and render options stay alive: the caller writes the streams first and then
// checks them in (binary order: RENDER, VAL count, VAL colours, CHECKIN, LRO dispose).
PF_Err ScanUpstreamRender(PF_InData *in, PF_OutData *out, AEGP_SuiteHandler &suites, AEGP_EffectRefH effectH,
                          const std::vector<AAAColor> &existing, std::vector<AAAColor> &found, size_t cap,
                          AEGP_LayerRenderOptionsH &opts, AEGP_FrameReceiptH &receipt) {
    opts = nullptr;
    receipt = nullptr;
    PF_Err err = suites.LayerRenderOptionsSuite2()->AEGP_NewFromUpstreamOfEffect(g_aegp_id, effectH,
                                                                                 &opts);
    if (err) return err;
    err = suites.LayerRenderOptionsSuite2()->AEGP_SetDownsampleFactor(opts, 1, 1);
    if (!err)
        err = suites.RenderSuite5()->AEGP_RenderAndCheckoutLayerFrame(opts, nullptr, nullptr,
                                                                      &receipt);
    AEGP_WorldH worldH = nullptr;
    PF_EffectWorld pfw;
    AEFX_CLR_STRUCT(pfw);
    if (!err && receipt) err = suites.RenderSuite5()->AEGP_GetReceiptWorld(receipt, &worldH);
    if (!err && worldH) err = suites.WorldSuite3()->AEGP_FillOutPFEffectWorld(worldH, &pfw);
    if (!err) {
        PF_PixelFormat fmt = PF_PixelFormat_ARGB32;
        AEFX_SuiteScoper<PF_WorldSuite2> world_suite(in, kPFWorldSuite, kPFWorldSuiteVersion2, out);
        err = world_suite->PF_GetPixelFormat(&pfw, &fmt);
        const int bpc = fmt == PF_PixelFormat_ARGB128 ? 32 : (fmt == PF_PixelFormat_ARGB64 ? 16 : 8);
        if (!err) ScanByFormat(&pfw, bpc, existing, found, cap);
    }
    return err;
}

// sub_180006BC0 init/add path. Free slots are colours >= count or black; new colours fill them in
// order; the count slider grows when the total exceeds it.
PF_Err AddAutomaticColours(PF_InData *in, PF_OutData *out, bool initialize) {
    AEGP_SuiteHandler suites(in->pica_basicP);
    AEGP_EffectRefH effectH = nullptr;
    PF_Err err = suites.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(g_aegp_id, in->effect_ref,
                                                                       &effectH);
    if (err || !effectH) return err;

    A_long count = 0;
    std::vector<AAAColor> existing;
    err = CheckoutSlider(in, OLMAAA_NUM_COLORS, count);
    if (count < 0) count = 0;
    if (count > OLMAAA_COLOR_COUNT) count = OLMAAA_COLOR_COUNT;
    if (!err) {
        if (initialize) {
            existing.assign(count, AAAColor{0, 0, 0, 0});  // colours were cleared just before
        } else {
            std::vector<AAAColor> cols;
            err = ReadColors(in, out, count, cols);
            existing = cols;
        }
    }
    if (!err) {
        // Slot selection (sub_180006BC0). INIT uses every slot in order. ADD (compaction): colours in
        // range that are not black count as used, and so does the FIRST black colour in range (it is not
        // a free slot); the other black colours in range and every slot >= count are free, ascending.
        std::vector<int> freeSlots;
        int used = 0;  // v25 in the binary
        if (initialize) {
            for (int i = 0; i < OLMAAA_COLOR_COUNT; ++i) freeSlots.push_back(i);
        } else {
            bool firstBlackSeen = false;
            for (int i = 0; i < OLMAAA_COLOR_COUNT; ++i) {
                if (i >= count) {
                    freeSlots.push_back(i);
                    continue;
                }
                const AAAColor &c = existing[i];
                if (c.r == 0.0f && c.g == 0.0f && c.b == 0.0f) {
                    if (!firstBlackSeen) {
                        firstBlackSeen = true;
                        ++used;
                    } else {
                        freeSlots.push_back(i);
                    }
                } else {
                    ++used;
                }
            }
        }
        std::vector<AAAColor> found;
        AEGP_LayerRenderOptionsH opts = nullptr;
        AEGP_FrameReceiptH receipt = nullptr;
        if (!freeSlots.empty())
            err = ScanUpstreamRender(in, out, suites, effectH, existing, found, freeSlots.size(), opts, receipt);
        // The scan is capped at the free-slot count. The count slider is written first (before the colour
        // streams) with value used + found, only when that exceeds the current count (v25 + v80 > v14).
        const int total = used + (int)found.size();
        if (!err && total > count) err = SetCountStream(suites, effectH, total);
        for (size_t k = 0; k < found.size() && !err; ++k)
            err = SetColorStream(suites, effectH, OLMAAA_COLOR_FIRST + freeSlots[k], found[k]);
        if (receipt) suites.RenderSuite5()->AEGP_CheckinFrame(receipt);
        if (opts) suites.LayerRenderOptionsSuite1()->AEGP_Dispose(opts);
    }
    suites.EffectSuite3()->AEGP_DisposeEffect(effectH);
    return err;
}

PF_Err UserChanged(PF_InData *in, PF_OutData *out, PF_UserChangedParamExtra *extra) {
    AEGP_SuiteHandler suites(in->pica_basicP);
    switch (extra->param_index) {
        case OLMAAA_INITIALIZE: {
            AEGP_EffectRefH effectH = nullptr;
            PF_Err err = suites.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(
                g_aegp_id, in->effect_ref, &effectH);
            if (err || !effectH) return err;
            err = ClearAllColours(in, suites, effectH);
            suites.EffectSuite3()->AEGP_DisposeEffect(effectH);
            if (err) return err;
            return AddAutomaticColours(in, out, true);
        }
        case OLMAAA_ADD_AUTO:
            return AddAutomaticColours(in, out, false);
        case OLMAAA_CLEAR: {
            AEGP_EffectRefH effectH = nullptr;
            PF_Err err = suites.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(
                g_aegp_id, in->effect_ref, &effectH);
            if (err || !effectH) return err;
            err = ClearAllColours(in, suites, effectH);
            suites.EffectSuite3()->AEGP_DisposeEffect(effectH);
            return err;
        }
        default:
            return PF_Err_NONE;
    }
}

// PF_Cmd_UPDATE_PARAMS_UI: colours with index >= Number of Colors are hidden.
PF_Err UpdateParamsUI(PF_InData *in, PF_OutData *out) {
    (void)out;
    A_long count = 0;
    PF_Err err = CheckoutSlider(in, OLMAAA_NUM_COLORS, count);
    if (err) return err;
    AEGP_SuiteHandler suites(in->pica_basicP);
    AEGP_EffectRefH effectH = nullptr;
    err = suites.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(g_aegp_id, in->effect_ref, &effectH);
    if (err || !effectH) return err;
    for (int i = 0; i < OLMAAA_COLOR_COUNT && !err; ++i)
        err = SetColorStreamHidden(suites, effectH, OLMAAA_COLOR_FIRST + i, i >= count);
    suites.EffectSuite3()->AEGP_DisposeEffect(effectH);
    return err;
}

// ---------------------------------------------------------------- params ---
void AddParam(PF_InData *in, PF_ParamDef &def, PF_ParamIndex pos, const char *name, PF_ParamType type,
              PF_ParamFlags flags, PF_ParamUIFlags uiFlags, PF_Err &err) {
    if (err) return;
    def.uu.id = pos;
    def.param_type = type;
    def.ui_flags = uiFlags;
    def.flags = flags;
    std::snprintf(def.PF_DEF_NAME, sizeof(def.PF_DEF_NAME), "%s", name ? name : "");
    err = in->inter.add_param(in->effect_ref, -1, &def);
}

PF_Err ParamsSetup(PF_InData *in, PF_OutData *out) {
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);

    // 1 Number of Colors: SLIDER 0..100, default 0
    def.u.sd.value = 0;
    def.u.sd.valid_min = 0;
    def.u.sd.valid_max = 100;
    def.u.sd.slider_min = 0;
    def.u.sd.slider_max = 100;
    def.u.sd.dephault = 0;
    AddParam(in, def, OLMAAA_NUM_COLORS, "Number of Colors", PF_Param_SLIDER, PF_ParamFlag_NONE, 0, err);

    // 2 Use automatic colors: CHECKBOX, default off
    AEFX_CLR_STRUCT(def);
    def.u.bd.value = 0;
    def.u.bd.dephault = 0;
    def.u.bd.u.PF_DEF_NAMEPTR = "";
    AddParam(in, def, OLMAAA_USE_AUTO, "Use automatic colors", PF_Param_CHECKBOX, PF_ParamFlag_NONE, 0, err);

    // 3-5 buttons (SUPERVISE, STD_CONTROL_ONLY); labels as in the binary
    const char *buttonLabels[3] = {"", "", "Clear"};
    const char *buttonNames[3] = {"Initalize colors", "Add automatic colors", "Clear colors list"};
    for (int b = 0; b < 3; ++b) {
        AEFX_CLR_STRUCT(def);
        def.u.button_d.u.PF_DEF_NAMESPTR = buttonLabels[b];
        AddParam(in, def, OLMAAA_INITIALIZE + b, buttonNames[b], PF_Param_BUTTON, PF_ParamFlag_SUPERVISE,
                 PF_PUI_STD_CONTROL_ONLY, err);
    }

    // 6 Colors list: group start (collapsed twirly)
    AEFX_CLR_STRUCT(def);
    AddParam(in, def, OLMAAA_COLORS_GROUP, "Colors list", PF_Param_GROUP_START, PF_ParamFlag_COLLAPSE_TWIRLY,
             0, err);

    // 7..106 colours "Color N" (black, opaque default), SUPERVISE
    for (int i = 0; i < OLMAAA_COLOR_COUNT && !err; ++i) {
        AEFX_CLR_STRUCT(def);
        def.u.cd.value.alpha = 255;
        def.u.cd.value.red = 0;
        def.u.cd.value.green = 0;
        def.u.cd.value.blue = 0;
        def.u.cd.dephault = def.u.cd.value;
        char name[32];
        std::snprintf(name, sizeof(name), "Color %d", i + 1);
        AddParam(in, def, OLMAAA_COLOR_FIRST + i, name, PF_Param_COLOR, PF_ParamFlag_SUPERVISE, 0, err);
    }

    // 107 group end
    AEFX_CLR_STRUCT(def);
    AddParam(in, def, OLMAAA_COLORS_GROUP_END, "", PF_Param_GROUP_END, PF_ParamFlag_NONE, 0, err);

    if (!err) out->num_params = OLMAAA_NUM_PARAMS;
    return err;
}

PF_Err GlobalSetup(PF_InData *in, PF_OutData *out) {
    out->my_version = OLMAAA_VERSION;
    out->out_flags = PF_OutFlag_USE_OUTPUT_EXTENT | PF_OutFlag_DEEP_COLOR_AWARE |
                     PF_OutFlag_SEND_UPDATE_PARAMS_UI;
    out->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |
                      PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    if (in->pica_basicP) {
        AEGP_SuiteHandler suites(in->pica_basicP);
        suites.UtilitySuite3()->AEGP_RegisterWithAEGP((AEGP_GlobalRefcon)0, OLMAAA_NAME, &g_aegp_id);
    }
    return PF_Err_NONE;
}

PF_Err About(PF_InData *in, PF_OutData *out) {
    (void)in;
    std::snprintf(out->return_msg, sizeof(out->return_msg), "%s v%d.%d.%d\r%s", OLMAAA_NAME, 1, 1, 6,
                  OLMAAA_ABOUT_DESC);
    return PF_Err_NONE;
}

}  // namespace

// Legacy PF_Cmd_RENDER is a documented no-op (see header note); SMART_RENDER does the work.
PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[],
                  PF_LayerDef *output, void *extra) {
    (void)params;
    (void)output;
    switch (cmd) {
        case PF_Cmd_ABOUT:
            return About(in_data, out_data);
        case PF_Cmd_GLOBAL_SETUP:
            return GlobalSetup(in_data, out_data);
        case PF_Cmd_PARAMS_SETUP:
            return ParamsSetup(in_data, out_data);
        case PF_Cmd_UPDATE_PARAMS_UI:
            return UpdateParamsUI(in_data, out_data);
        case PF_Cmd_USER_CHANGED_PARAM:
            return UserChanged(in_data, out_data, (PF_UserChangedParamExtra *)extra);
        case PF_Cmd_SMART_PRE_RENDER:
            return PreRender(in_data, out_data, (PF_PreRenderExtra *)extra);
        case PF_Cmd_SMART_RENDER:
            return RenderPixels(in_data, out_data, (PF_SmartRenderExtra *)extra);
        default:
            return PF_Err_NONE;
    }
}

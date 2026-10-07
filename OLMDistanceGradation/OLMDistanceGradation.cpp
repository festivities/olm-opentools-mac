/* OLMDistanceGradation: macOS SmartRender port of the Windows OLM Distance
 * Gradation 0.8.2 (DistanceGradation.aex). The binary statically links
 * OpenCV 4.5.5 and drives it through the old C API; the handful of routines
 * it uses are reimplemented below following OpenCV's arithmetic.
 *
 * Binary map: entry 0x181174BC0, params 0x181173720, UI 0x181174190 ->
 * 0x1811743A0 (+ disable helper 0x181174050), smart render 0x181173D10,
 * compute 0x181171D00 (8) / 0x181170FF0 (16) / 0x181172A10 (32), mask
 * 0x181174990, invert 0x181174AC0, gradation 0x181174750, composite pixel
 * functions 0x181170870 (8) / 0x181170480 (16) / 0x181170C90 (32).
 * OpenCV callees: cvSplit 0x18117CC30, cvConvertScale 0x18117C570,
 * cvThreshold 0x1812B6A30, cvResize 0x1812AEF60, cvDistTransform 0x1812B1590,
 * cvNormalize 0x18117CA40, cvAdd 0x181182B10, cvCountNonZero 0x181182FF0,
 * cvSmooth 0x1812864C0, cvMerge 0x18117C6D0. */

#include "OLMDistanceGradation.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

namespace {

static_assert(PF_VERSION(OLMDG_MAJOR_VERSION, OLMDG_MINOR_VERSION, OLMDG_BUG_VERSION,
                         PF_Stage_ALPHA, 0) == 266752,
              "DistanceGradation version must match its PiPL");

PF_Err About(PF_InData *in_data, PF_OutData *out_data) {
    PF_SPRINTF(out_data->return_msg, "%s", OLMDG_ABOUT);
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_OutData *out_data) {
    out_data->my_version = PF_VERSION(OLMDG_MAJOR_VERSION, OLMDG_MINOR_VERSION,
                                      OLMDG_BUG_VERSION, PF_Stage_ALPHA, 0);
    out_data->out_flags = PF_OutFlag_USE_OUTPUT_EXTENT | PF_OutFlag_DEEP_COLOR_AWARE |
                          PF_OutFlag_SEND_UPDATE_PARAMS_UI;
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER |
                           PF_OutFlag2_FLOAT_COLOR_AWARE |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    // The binary also registers with AEGP, but only for UI calls whose results
    // it discards; the port skips both.
    return PF_Err_NONE;
}

PF_Err AddCheckbox(PF_InData *in_data, const char *name, A_long id) {
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_CHECKBOX;
    PF_STRNNCPY(def.PF_DEF_NAME, name, sizeof(def.PF_DEF_NAME));
    def.u.bd.u.nameptr = name;
    def.uu.id = id;
    return PF_ADD_PARAM(in_data, -1, &def);
}

// Popups are added raw because In/Out ships value = dephault = 0.
PF_Err AddPopup(PF_InData *in_data, const char *name, A_short choices, A_short dflt,
                const char *items, A_long id) {
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_POPUP;
    PF_STRNNCPY(def.PF_DEF_NAME, name, sizeof(def.PF_DEF_NAME));
    def.u.pd.value = dflt;
    def.u.pd.num_choices = choices;
    def.u.pd.dephault = dflt;
    def.u.pd.u.namesptr = items;
    def.uu.id = id;
    return PF_ADD_PARAM(in_data, -1, &def);
}

PF_Err ParamsSetup(PF_InData *in_data, PF_OutData *out_data) {
    PF_ParamDef def;
    PF_Err err = AddCheckbox(in_data, "Invert", OLMDG_INVERT);
    if (!err) err = AddPopup(in_data, "In/Out", 3, 0, "Inside|Outside|Both", OLMDG_IN_OUT);
    if (err) return err;
    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Inside Threshold", 0, 1000, 0, 512, 128, OLMDG_INSIDE_THR);
    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Outside Threshold", 0, 1000, 0, 512, 128, OLMDG_OUTSIDE_THR);
    err = AddPopup(in_data, "Render Mode", 2, 1, "RGB|Layer", OLMDG_RENDER_MODE);
    if (!err) err = AddCheckbox(in_data, "Use Background Color", OLMDG_USE_BG);
    if (err) return err;
    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Gradation Color", 255, 0, 0, OLMDG_GRAD_COLOR);
    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("BG Color ", 0, 0, 0, OLMDG_BG_COLOR);  // trailing space is in the binary
    err = AddPopup(in_data, "Interpolation Mode", 4, OLMDG_LINEAR,
                   "Constant|Linear|Sphere|Power", OLMDG_INTERP);
    if (err) return err;
    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_COLLAPSE_TWIRLY;
    PF_ADD_FLOAT_SLIDER("Power", 0.01, 5, 0.01, 5, 0, 1, 2, PF_ValueDisplayFlag_NONE, false,
                        OLMDG_POWER);
    err = AddPopup(in_data, "Blur Mode", 3, OLMDG_NO_BLUR, "No Blur|Blur No Scale|Blur",
                   OLMDG_BLUR_MODE);
    if (err) return err;
    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_COLLAPSE_TWIRLY;
    PF_ADD_SLIDER("Blur Size", 0, 4096, 0, 500, 0, OLMDG_BLUR_SIZE);
    out_data->num_params = OLMDG_NUM_PARAMS;
    return PF_Err_NONE;
}

// --- UPDATE_PARAMS_UI (0x1811743A0) -------------------------------------------

PF_Err SetDisabled(AEFX_SuiteScoper<PF_ParamUtilsSuite3> &suite, PF_InData *in_data,
                   PF_ParamDef *params[], PF_ParamIndex index, bool disabled) {
    if (!params[index]) return PF_Err_NONE;
    PF_ParamDef def = *params[index];  // a copy of the live definition
    if (disabled) def.ui_flags |= PF_PUI_DISABLED;
    else def.ui_flags &= ~((A_long)PF_PUI_DISABLED);
    return suite->PF_UpdateParamUI(in_data->effect_ref, index, &def);
}

PF_Err UpdateParamsUI(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[]) {
    if (!params) return PF_Err_NONE;
    auto value = [&](int i) { return params[i] ? params[i]->u.pd.value : 0; };
    AEFX_SuiteScoper<PF_ParamUtilsSuite3> suite(in_data, kPFParamUtilsSuite,
                                                kPFParamUtilsSuiteVersion3, out_data);
    const A_long in_out = value(OLMDG_IN_OUT);
    PF_Err err = SetDisabled(suite, in_data, params, OLMDG_POWER,
                             value(OLMDG_INTERP) != OLMDG_POWER_MODE);
    if (!err) err = SetDisabled(suite, in_data, params, OLMDG_OUTSIDE_THR, in_out == OLMDG_INSIDE);
    if (!err) err = SetDisabled(suite, in_data, params, OLMDG_INSIDE_THR, in_out == OLMDG_OUTSIDE);
    if (!err)
        err = SetDisabled(suite, in_data, params, OLMDG_GRAD_COLOR,
                          value(OLMDG_RENDER_MODE) != OLMDG_RENDER_RGB);
    if (!err)
        err = SetDisabled(suite, in_data, params, OLMDG_BG_COLOR,
                          !(params[OLMDG_USE_BG] && params[OLMDG_USE_BG]->u.bd.value));
    if (!err)
        err = SetDisabled(suite, in_data, params, OLMDG_BLUR_SIZE,
                          value(OLMDG_BLUR_MODE) == OLMDG_NO_BLUR);
    return err;
}

PF_Err PreRender(PF_InData *in_data, PF_PreRenderExtra *extra) {
    PF_CheckoutResult result;
    AEFX_CLR_STRUCT(result);
    PF_RenderRequest request = extra->input->output_request;
    PF_Err err = extra->cb->checkout_layer(in_data->effect_ref, OLMDG_INPUT, 0, &request,
                                           in_data->current_time, in_data->time_step,
                                           in_data->time_scale, &result);
    if (!err) {
        UnionLRect(&result.result_rect, &extra->output->result_rect);
        UnionLRect(&result.max_result_rect, &extra->output->max_result_rect);
    }
    return err;
}

PF_Err CheckoutParams(PF_InData *in_data, PF_OutData *out_data, OLMDGParams *p) {
    AEFX_SuiteScoper<PF_ColorParamSuite1> color_suite(in_data, kPFColorParamSuite,
                                                      kPFColorParamSuiteVersion1, out_data);
    PF_Err err = PF_Err_NONE;
    for (int i = 1; i < OLMDG_NUM_PARAMS && !err; ++i) {
        PF_ParamDef def;
        AEFX_CLR_STRUCT(def);
        err = PF_CHECKOUT_PARAM(in_data, i, in_data->current_time, in_data->time_step,
                                in_data->time_scale, &def);
        if (err) break;
        switch (i) {
            case OLMDG_INVERT: p->invert = def.u.bd.value != 0; break;
            case OLMDG_IN_OUT: p->in_out = def.u.pd.value; break;
            case OLMDG_INSIDE_THR: p->inside_thr = def.u.sd.value; break;
            case OLMDG_OUTSIDE_THR: p->outside_thr = def.u.sd.value; break;
            case OLMDG_RENDER_MODE: p->render_mode = def.u.pd.value; break;
            case OLMDG_USE_BG: p->use_bg = def.u.bd.value != 0; break;
            case OLMDG_GRAD_COLOR:
                err = color_suite->PF_GetFloatingPointColorFromColorDef(in_data->effect_ref,
                                                                        &def, &p->grad_color);
                break;
            case OLMDG_BG_COLOR:
                err = color_suite->PF_GetFloatingPointColorFromColorDef(in_data->effect_ref,
                                                                        &def, &p->bg_color);
                break;
            case OLMDG_INTERP: p->interp = def.u.pd.value; break;
            case OLMDG_POWER: p->power = (float)def.u.fs_d.value; break;
            case OLMDG_BLUR_MODE: p->blur_mode = def.u.pd.value; break;
            case OLMDG_BLUR_SIZE: p->blur_size = def.u.sd.value; break;
        }
        const PF_Err checkin_err = PF_CHECKIN_PARAM(in_data, &def);
        if (!err) err = checkin_err;
    }
    return err;
}

// --- OpenCV 4.5.5 equivalents (single-channel planes) -------------------------

using Plane = std::vector<float>;
using Mask = std::vector<unsigned char>;

// cvResize(INTER_NEAREST): sx = min(floor(x * src/dst), src-1).
Mask ResizeNearest(const Mask &src, int sw, int sh, int dw, int dh) {
    if (sw == dw && sh == dh) return src;
    Mask dst((size_t)dw * dh);
    const double ifx = 1.0 / ((double)dw / sw), ify = 1.0 / ((double)dh / sh);
    std::vector<int> xo(dw);
    for (int x = 0; x < dw; ++x) xo[x] = std::min((int)std::floor(x * ifx), sw - 1);
    for (int y = 0; y < dh; ++y) {
        const int sy = std::min((int)std::floor(y * ify), sh - 1);
        for (int x = 0; x < dw; ++x) dst[(size_t)y * dw + x] = src[(size_t)sy * sw + xo[x]];
    }
    return dst;
}

// cvResize(INTER_LINEAR) for float: centre-aligned taps, edge-clamped.
Plane ResizeLinear(const Plane &src, int sw, int sh, int dw, int dh) {
    if (sw == dw && sh == dh) return src;
    struct Tap { int i0, i1; float w0, w1; };
    auto taps = [](int s, int d) {
        std::vector<Tap> t(d);
        const double scale = (double)s / d;
        for (int i = 0; i < d; ++i) {
            float f = (float)((i + 0.5) * scale - 0.5);
            int si = (int)std::floor(f);
            f -= si;
            if (si < 0) f = 0, si = 0;
            if (si >= s - 1) f = 0, si = s - 1;
            t[i] = {si, std::min(si + 1, s - 1), 1.0f - f, f};
        }
        return t;
    };
    const std::vector<Tap> tx = taps(sw, dw), ty = taps(sh, dh);
    Plane rows((size_t)sh * dw);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < dw; ++x) {
            const float *r = &src[(size_t)y * sw];
            rows[(size_t)y * dw + x] = r[tx[x].i0] * tx[x].w0 + r[tx[x].i1] * tx[x].w1;
        }
    Plane dst((size_t)dw * dh);
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x)
            dst[(size_t)y * dw + x] = ty[y].w0 * rows[(size_t)ty[y].i0 * dw + x] +
                                      ty[y].w1 * rows[(size_t)ty[y].i1 * dw + x];
    return dst;
}

// cvDistTransform(CV_DIST_L2, CV_DIST_MASK_PRECISE) = trueDistTrans: exact
// Euclidean distance from each nonzero pixel to the nearest zero pixel. The
// image border is not a zero: a column without zeros is "infinite" (1e15).
Plane DistanceTransform(const Mask &src, int n, int m) {
    const float inf = 1e15f;
    Plane d((size_t)n * m);
    std::vector<int> col(m);
    for (int x = 0; x < n; ++x) {
        int dist = m - 1;
        for (int y = m - 1; y >= 0; --y) {
            dist = src[(size_t)y * n + x] ? dist + 1 : 0;
            col[y] = dist;
        }
        dist = m - 1;
        for (int y = 0; y < m; ++y) {
            dist = std::min(dist + 1, col[y]);
            d[(size_t)y * n + x] = dist >= m ? inf : (float)(dist * dist);
        }
    }
    std::vector<float> sqr(n), inv(n), f(n), z(n + 1);
    std::vector<int> v(n);
    for (int i = 0; i < n; ++i) {
        sqr[i] = (float)(i * i);
        inv[i] = i ? (float)(0.5 / i) : 0.0f;
    }
    for (int y = 0; y < m; ++y) {
        float *row = &d[(size_t)y * n];
        int k = 0;
        v[0] = 0;
        z[0] = -inf;
        z[1] = inf;
        f[0] = row[0];
        for (int q = 1; q < n; ++q) {
            const float fq = row[q];
            f[q] = fq;
            for (;; --k) {
                const int p = v[k];
                const float s = (fq + sqr[q] - row[p] - sqr[p]) * inv[q - p];
                if (s > z[k]) {
                    ++k;
                    v[k] = q;
                    z[k] = s;
                    z[k + 1] = inf;
                    break;
                }
            }
        }
        k = 0;
        for (int q = 0; q < n; ++q) {
            while (z[k + 1] < q) ++k;
            const int p = v[k];
            row[q] = std::sqrt(sqr[std::abs(q - p)] + f[p]);
        }
    }
    return d;
}

// cvNormalize(CV_MINMAX) to [0, maxval] with OpenCV's float scale/shift.
void NormalizeMinMax(Plane &p, double maxval) {
    if (p.empty()) return;
    double smin = p[0], smax = p[0];
    for (float v : p) smin = std::min(smin, (double)v), smax = std::max(smax, (double)v);
    double scale = maxval * (smax - smin > DBL_EPSILON ? 1.0 / (smax - smin) : 0.0);
    scale = (float)scale;
    const float a = (float)scale, b = 0.0f - (float)(smin * scale);
    for (float &v : p) v = v * a + b;
}

// cvSmooth CV_BLUR: normalized box filter, BORDER_REPLICATE.
Plane BoxBlur(const Plane &src, int w, int h, int kx, int ky) {
    const int ax = kx / 2, ay = ky / 2;
    std::vector<double> rows((size_t)w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double s = 0;
            for (int i = 0; i < kx; ++i)
                s += src[(size_t)y * w + std::clamp(x + i - ax, 0, w - 1)];
            rows[(size_t)y * w + x] = s;
        }
    Plane dst((size_t)w * h);
    const double scale = 1.0 / ((double)kx * ky);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double s = 0;
            for (int i = 0; i < ky; ++i) s += rows[(size_t)std::clamp(y + i - ay, 0, h - 1) * w + x];
            dst[(size_t)y * w + x] = (float)(s * scale);
        }
    return dst;
}

// getGaussianKernel(n, sigma <= 0, CV_32F).
std::vector<float> GaussianKernel(int n) {
    static const float small[4][7] = {
        {1.f},
        {0.25f, 0.5f, 0.25f},
        {0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f},
        {0.03125f, 0.109375f, 0.21875f, 0.28125f, 0.21875f, 0.109375f, 0.03125f}};
    const float *fixed = (n % 2 == 1 && n <= 7) ? small[n >> 1] : nullptr;
    const double sigma = ((n - 1) * 0.5 - 1) * 0.3 + 0.8;
    const double scale2 = -0.5 / (sigma * sigma);
    std::vector<float> k(n);
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        const double x = i - (n - 1) * 0.5;
        k[i] = (float)(fixed ? (double)fixed[i] : std::exp(scale2 * x * x));
        sum += k[i];
    }
    sum = 1.0 / sum;
    for (float &v : k) v = (float)(v * sum);
    return k;
}

// cvSmooth CV_GAUSSIAN (sigma 0 -> derived from size), BORDER_REPLICATE.
Plane GaussianBlur(const Plane &src, int w, int h, int kx, int ky) {
    if (kx == 1 && ky == 1) return src;
    const std::vector<float> gx = GaussianKernel(kx), gy = GaussianKernel(ky);
    const int ax = kx / 2, ay = ky / 2;
    Plane rows((size_t)w * h), dst((size_t)w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float s = 0;
            for (int i = 0; i < kx; ++i)
                s += gx[i] * src[(size_t)y * w + std::clamp(x + i - ax, 0, w - 1)];
            rows[(size_t)y * w + x] = s;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float s = 0;
            for (int i = 0; i < ky; ++i)
                s += gy[i] * rows[(size_t)std::clamp(y + i - ay, 0, h - 1) * w + x];
            dst[(size_t)y * w + x] = s;
        }
    return dst;
}

// --- Gradation pipeline (0x181174750) ---------------------------------------

struct Geometry {
    int w, h;    // layer (possibly downsampled)
    int fw, fh;  // full resolution: distances are measured in full-res pixels
};

Plane Gradation(const Mask &mask, const Geometry &g, A_long thr, A_long interp, double maxval) {
    const Mask full = ResizeNearest(mask, g.w, g.h, g.fw, g.fh);
    Plane d = ResizeLinear(DistanceTransform(full, g.fw, g.fh), g.fw, g.fh, g.w, g.h);
    const float t = (float)thr;
    if (interp == OLMDG_CONSTANT) {
        const float T = std::max(1.0f, t);  // cvThreshold BINARY, maxval = T
        for (float &v : d) v = v > T ? T : 0.0f;
    } else {
        const float T = t == 0.0f ? 0.1f : t;  // cvThreshold TRUNC
        for (float &v : d) v = v > T ? T : v;
    }
    NormalizeMinMax(d, maxval);
    return d;
}

// --- Pixel I/O ------------------------------------------------------------------

template <typename P> struct Depth;
template <> struct Depth<PF_Pixel> {
    static constexpr double kMax = 255.0;
    static float Norm(A_u_char v) { return (float)v / 255.0f; }
    static A_u_char Out(float v) { return (A_u_char)(int)(float)(v * 255.0f); }
    static A_u_char Sat(float v) { return (A_u_char)std::clamp(std::lrint(v), 0L, 255L); }
    static A_u_char Full() { return 255; }
    static A_u_char Alpha8(A_u_char a) { return a; }
};
template <> struct Depth<PF_Pixel16> {
    static constexpr double kMax = 32768.0;
    static float Norm(A_u_short v) { return (float)v * 0.000030517578f; }
    static A_u_short Out(float v) { return (A_u_short)(int)(float)(v * 32768.0f); }
    static A_u_short Sat(float v) { return (A_u_short)std::clamp(std::lrint(v), 0L, 65535L); }
    static A_u_short Full() { return 0x8000; }
    static A_u_char Alpha8(A_u_short a) {  // cvConvertScale(.., 1/128)
        return (A_u_char)std::clamp(std::lrint(a * 0.0078125f), 0L, 255L);
    }
};
template <> struct Depth<PF_PixelFloat> {
    static constexpr double kMax = 1.0;
    static float Norm(PF_FpShort v) { return v; }
    static PF_FpShort Out(float v) { return v; }
    static PF_FpShort Sat(float v) { return v; }
    static PF_FpShort Full() { return 1.0f; }
    static A_u_char Alpha8(PF_FpShort a) {  // cvConvertScale(.., 255)
        return (A_u_char)std::clamp(std::lrint(a * 255.0f), 0L, 255L);
    }
};

template <typename P>
P *At(PF_EffectWorld *w, int x, int y) {
    return (P *)((char *)w->data + (ptrdiff_t)y * w->rowbytes) + x;
}

// Composite (0x181170480 family). `g` holds the gradation the compute step
// wrote into the output world: alpha/red/green = blurred, blue = unblurred.
template <typename P>
P Composite(const OLMDGParams &p, const P &in, const P &g) {
    using D = Depth<P>;
    float r = D::Norm(g.red), gg = D::Norm(g.green), b = D::Norm(g.blue);
    float t = p.invert ? r : 1.0f - r;
    float a = 0.0f, alpha_w = D::Norm(in.alpha);
    bool composite = true;
    if (p.in_out == OLMDG_OUTSIDE) {
        alpha_w = std::fmin(1.0f, 1.0f - alpha_w);
        if (alpha_w < 0.0f) composite = false;
    } else if (p.in_out == OLMDG_BOTH) {
        alpha_w = 1.0f;
    }
    if (composite && p.in_out != OLMDG_BOTH && !(alpha_w >= 0.0001f)) composite = false;
    if (composite) {
        if (p.interp == OLMDG_SPHERE) {
            t = (float)std::sqrt(1.0 - std::pow((double)(float)(1.0f - t), 2.0));
        } else if (p.interp == OLMDG_POWER_MODE) {
            t = powf(t, p.power);
        }
        float cr = 1, cg = 1, cb = 1;
        if (p.render_mode == OLMDG_RENDER_RGB) {
            cr = p.grad_color.red, cg = p.grad_color.green, cb = p.grad_color.blue;
        } else if (p.render_mode == OLMDG_RENDER_LAYER) {
            cr = D::Norm(in.red), cg = D::Norm(in.green), cb = D::Norm(in.blue);
        }
        if (!p.use_bg) {
            r = cr, gg = cg, b = cb;
            a = alpha_w * t;
        } else {
            float br = 0, bgc = 0, bb = 0;
            if (p.render_mode == OLMDG_RENDER_RGB || p.render_mode == OLMDG_RENDER_LAYER)
                br = p.bg_color.red, bgc = p.bg_color.green, bb = p.bg_color.blue;
            r = (1.0f - t) * br + cr * t;
            gg = (1.0f - t) * bgc + cg * t;
            b = (1.0f - t) * bb + cb * t;
            a = alpha_w;
        }
    }
    // When not composited the raw gradation channels are written with alpha 0.
    P out;
    out.alpha = D::Out(a);
    out.red = D::Out(r);
    out.green = D::Out(gg);
    out.blue = D::Out(b);
    return out;
}

template <typename P>
void Render(PF_InData *in_data, PF_EffectWorld *input, PF_EffectWorld *output,
            const OLMDGParams &p) {
    using D = Depth<P>;
    Geometry g;
    g.w = input->width;
    g.h = input->height;
    if (g.w <= 0 || g.h <= 0) return;
    g.fw = (int)((float)(g.w * (A_long)in_data->downsample_x.den) /
                 (float)in_data->downsample_x.num);
    g.fh = (int)((float)(g.h * (A_long)in_data->downsample_y.den) /
                 (float)in_data->downsample_y.num);
    g.fw = std::max(g.fw, 1);
    g.fh = std::max(g.fh, 1);
    const size_t n = (size_t)g.w * g.h;

    // 0x181174990: alpha -> 8-bit, cvThreshold(> 1.0, 255, BINARY).
    Mask mask(n);
    size_t on = 0;
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x) {
            const bool set = D::Alpha8(At<P>(input, x, y)->alpha) > 1;
            mask[(size_t)y * g.w + x] = set ? 255 : 0;
            on += set;
        }
    const bool degenerate = on == n || on == 0;

    Plane dist(n, 0.0f);
    switch (p.in_out) {
        case OLMDG_INSIDE:
            if (p.inside_thr == 0) {
                for (size_t i = 0; i < n; ++i) dist[i] = mask[i];
                NormalizeMinMax(dist, D::kMax);
            } else {
                dist = Gradation(mask, g, p.inside_thr, p.interp, D::kMax);
            }
            break;
        case OLMDG_OUTSIDE:
            for (auto &m : mask) m = 255 - m;  // 0x181174AC0
            dist = Gradation(mask, g, p.outside_thr, p.interp, D::kMax);
            break;
        case OLMDG_BOTH: {
            const Plane inside = Gradation(mask, g, p.inside_thr, p.interp, D::kMax);
            for (auto &m : mask) m = 255 - m;
            dist = Gradation(mask, g, p.outside_thr, p.interp, D::kMax);
            for (size_t i = 0; i < n; ++i) dist[i] = inside[i] + dist[i];  // cvAdd
            break;
        }
        default:
            break;  // binary leaves its (uninitialised) buffer; the port uses 0
    }

    Plane blurred = dist;
    if (p.blur_mode != OLMDG_NO_BLUR) {
        // cvSmooth(type = mode - 1): 1 = CV_BLUR (normalized box), 2 = CV_GAUSSIAN.
        const unsigned size = (unsigned)p.blur_size;
        const int kx = 2 * (int)(((unsigned)in_data->downsample_x.num * size) /
                                 (unsigned)in_data->downsample_x.den) + 1;
        const int ky = 2 * (int)(((unsigned)in_data->downsample_y.num * size) /
                                 (unsigned)in_data->downsample_y.den) + 1;
        if (p.blur_mode == OLMDG_BLUR_NO_SCALE) blurred = BoxBlur(dist, g.w, g.h, kx, ky);
        else if (p.blur_mode == OLMDG_BLUR) blurred = GaussianBlur(dist, g.w, g.h, kx, ky);
    }

    // cvMerge(blurred x3, dist) + cvConvert into the output world.
    const int ow = std::min(g.w, (int)output->width), oh = std::min(g.h, (int)output->height);
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x) {
            P &o = *At<P>(output, x, y);
            const size_t i = (size_t)y * g.w + x;
            o.alpha = o.red = o.green = D::Sat(blurred[i]);
            o.blue = D::Sat(dist[i]);
        }

    // Composite over the output extent.
    const PF_Rect &e = output->extent_hint;
    for (int y = std::max(0, (int)e.top); y < std::min((int)e.bottom, oh); ++y)
        for (int x = std::max(0, (int)e.left); x < std::min((int)e.right, ow); ++x) {
            P &o = *At<P>(output, x, y);
            if (degenerate) {
                if (p.use_bg) {
                    o.alpha = D::Full();
                    o.red = D::Out(p.bg_color.red);
                    o.green = D::Out(p.bg_color.green);
                    o.blue = D::Out(p.bg_color.blue);
                } else {
                    o.alpha = o.red = o.green = o.blue = 0;
                }
            } else {
                o = Composite<P>(p, *At<P>(input, x, y), o);
            }
        }
}

PF_Err SmartRender(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra) {
    const int depth = extra->input->bitdepth;
    if (depth != 8 && depth != 16 && depth != 32) return PF_Err_NONE;
    PF_EffectWorld *input = nullptr;
    PF_EffectWorld *output = nullptr;
    PF_Err err = extra->cb->checkout_layer_pixels(in_data->effect_ref, OLMDG_INPUT, &input);
    const bool checked_out = !err;
    if (!err) err = extra->cb->checkout_output(in_data->effect_ref, &output);
    if (!err && (!input || !output)) err = PF_Err_BAD_CALLBACK_PARAM;
    try {
        OLMDGParams params{};
        if (!err) err = CheckoutParams(in_data, out_data, &params);
        if (!err) {
            if (depth == 8) Render<PF_Pixel>(in_data, input, output, params);
            else if (depth == 16) Render<PF_Pixel16>(in_data, input, output, params);
            else Render<PF_PixelFloat>(in_data, input, output, params);
        }
    } catch (...) {
        if (checked_out) extra->cb->checkin_layer_pixels(in_data->effect_ref, OLMDG_INPUT);
        throw;
    }
    if (checked_out) {
        const PF_Err checkin_err =
            extra->cb->checkin_layer_pixels(in_data->effect_ref, OLMDG_INPUT);
        if (!err) err = checkin_err;
    }
    return err;
}

}  // namespace

PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
                  PF_ParamDef *params[], PF_LayerDef *output, void *extra) {
    (void)output;
    try {
        switch (cmd) {
            case PF_Cmd_ABOUT:
                return About(in_data, out_data);
            case PF_Cmd_GLOBAL_SETUP:
                return GlobalSetup(out_data);
            case PF_Cmd_PARAMS_SETUP:
                return ParamsSetup(in_data, out_data);
            case PF_Cmd_UPDATE_PARAMS_UI:
                return UpdateParamsUI(in_data, out_data, params);
            case PF_Cmd_SMART_PRE_RENDER:
                return PreRender(in_data, (PF_PreRenderExtra *)extra);
            case PF_Cmd_SMART_RENDER:
                return SmartRender(in_data, out_data, (PF_SmartRenderExtra *)extra);
            case PF_Cmd_RENDER:  // legacy path not used under SmartRender
            default:
                return PF_Err_NONE;
        }
    } catch (PF_Err err) {
        return err;
    } catch (const std::bad_alloc &) {
        return PF_Err_OUT_OF_MEMORY;
    } catch (...) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
}

/* OLMBlur: macOS SmartRender port of the Windows OLM Blur 1.2.1 (OLMBlur.aex).
 * Follows the decompiled binary: entry 0x18000A970, params 0x180009EC0,
 * UI 0x18000A750, checkout 0x180009B40, smart render 0x18000A380,
 * kernels 0x180003710/2280/4B80 (current) and 0x180007300/5F20/86D0 (Legacy),
 * passes 0x180001000/1980 (current) and 0x1800014F0/1EA0 (Legacy),
 * Legacy LUT 0x180009E10, rect union 0x18000BD90. */

#include "OLMBlur.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <vector>

namespace {

static_assert(PF_VERSION(OLMBLUR_MAJOR_VERSION, OLMBLUR_MINOR_VERSION, OLMBLUR_BUG_VERSION,
                         PF_Stage_DEVELOP, 0) == 591872,
              "Blur version must match its PiPL");

AEGP_PluginID g_aegp_id = 0;

PF_Err About(PF_InData *in_data, PF_OutData *out_data) {
    PF_SPRINTF(out_data->return_msg, "%s", OLMBLUR_ABOUT);
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData *in_data, PF_OutData *out_data) {
    out_data->my_version = PF_VERSION(OLMBLUR_MAJOR_VERSION, OLMBLUR_MINOR_VERSION,
                                      OLMBLUR_BUG_VERSION, PF_Stage_DEVELOP, 0);
    out_data->out_flags = PF_OutFlag_USE_OUTPUT_EXTENT | PF_OutFlag_DEEP_COLOR_AWARE |
                          PF_OutFlag_SEND_UPDATE_PARAMS_UI;
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER |
                           PF_OutFlag2_FLOAT_COLOR_AWARE |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    // The binary never registers and passes plug-in ID 0 to the AEGP calls;
    // the port registers like its siblings.
    if (in_data->pica_basicP) {
        AEGP_SuiteHandler suites(in_data->pica_basicP);
        suites.UtilitySuite3()->AEGP_RegisterWithAEGP(nullptr, OLMBLUR_NAME, &g_aegp_id);
    }
    return PF_Err_NONE;
}

PF_Err ParamsSetup(PF_InData *in_data, PF_OutData *out_data) {
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDER("Blur Amount", 1, 1000, 1, 50, 0, 5, 2, PF_ValueDisplayFlag_NONE,
                        false, 5);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FIXED("Blur Smoothness", 1, 100, 1, 100, 100, 1, PF_ValueDisplayFlag_PERCENT, 0, 6);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Number of Repeat", 1, 10, 1, 10, 2, 3);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Bias Direction", 2, OLMBLUR_BIAS_VERTICAL, "Vertical|Horizontal", 4);

    // New instances default to off; projects saved before Legacy existed get
    // value 1 (USE_VALUE_FOR_OLD_PROJECTS), selecting the old algorithm.
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_CHECKBOX;
    def.flags = PF_ParamFlag_USE_VALUE_FOR_OLD_PROJECTS;
    PF_STRNNCPY(def.PF_DEF_NAME, "Legacy", sizeof(def.PF_DEF_NAME));
    def.u.bd.value = 1;
    def.u.bd.dephault = 0;
    def.u.bd.u.nameptr = "Legacy";
    def.uu.id = 7;
    PF_Err err = PF_ADD_PARAM(in_data, -1, &def);
    if (!err) out_data->num_params = OLMBLUR_NUM_PARAMS;
    return err;
}

// UPDATE_PARAMS_UI: Blur Smoothness is unconditionally hidden.
PF_Err UpdateParamsUI(PF_InData *in_data) {
    if (!in_data->pica_basicP) return PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    AEGP_EffectRefH effectH = nullptr;
    PF_Err err = suites.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(
        g_aegp_id, in_data->effect_ref, &effectH);
    if (err || !effectH) return err;
    AEGP_StreamRefH streamH = nullptr;
    if (!suites.StreamSuite7()->AEGP_GetNewEffectStreamByIndex(g_aegp_id, effectH,
                                                               OLMBLUR_SMOOTHNESS, &streamH) &&
        streamH) {
        suites.DynamicStreamSuite3()->AEGP_SetDynamicStreamFlag(
            streamH, AEGP_DynStreamFlag_HIDDEN, FALSE, TRUE);
        suites.StreamSuite7()->AEGP_DisposeStream(streamH);
    }
    return suites.EffectSuite5()->AEGP_DisposeEffect(effectH);
}

// PreRender: request the whole layer and return extra pixels.
PF_Err PreRender(PF_InData *in_data, PF_PreRenderExtra *extra) {
    PF_RenderRequest request = extra->input->output_request;
    PF_LRect full = {0, 0, in_data->width, in_data->height};
    UnionLRect(&full, &request.rect);
    PF_CheckoutResult result;
    AEFX_CLR_STRUCT(result);
    PF_Err err = extra->cb->checkout_layer(in_data->effect_ref, OLMBLUR_INPUT, 0, &request,
                                           in_data->current_time, in_data->time_step,
                                           in_data->time_scale, &result);
    extra->output->flags = PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS;
    if (!err) {
        UnionLRect(&result.result_rect, &extra->output->result_rect);
        UnionLRect(&result.max_result_rect, &extra->output->max_result_rect);
    }
    return err;
}

PF_Err CheckoutParams(PF_InData *in_data, OLMBlurParams *p) {
    PF_Err err = PF_Err_NONE;
    for (int i = OLMBLUR_AMOUNT; i < OLMBLUR_NUM_PARAMS && !err; ++i) {
        PF_ParamDef def;
        AEFX_CLR_STRUCT(def);
        err = PF_CHECKOUT_PARAM(in_data, i, in_data->current_time, in_data->time_step,
                                in_data->time_scale, &def);
        if (err) break;
        switch (i) {
            case OLMBLUR_AMOUNT: p->amount = (float)def.u.fs_d.value; break;
            case OLMBLUR_SMOOTHNESS: p->smoothness = (float)(A_short)(def.u.fd.value >> 16); break;
            case OLMBLUR_REPEAT: p->repeat = def.u.sd.value; break;
            case OLMBLUR_BIAS: p->bias = def.u.pd.value; break;
            case OLMBLUR_LEGACY: p->legacy = def.u.bd.value != 0; break;
        }
        err = PF_CHECKIN_PARAM(in_data, &def);
    }
    return err;
}

// --- Blur passes on an RGB float canvas (3 floats per pixel) ---------------
// mask[p] = input alpha != 0. Taps never cross a zero-alpha pixel, so separate
// opaque regions blur independently ("keeping the selected divisions").

struct Canvas {
    int w, h;
    std::vector<unsigned char> mask;
    std::vector<float> a, b;  // ping-pong RGB buffers
};

inline void Copy3(float *d, const float *s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }

// sub_180001000 (step 1, along rows) / sub_180001980 (step w, along columns).
// pos/len: the pixel's coordinate along the pass and the line length.
void Pass(const Canvas &c, const float *src, float *dst, const float *lut, int radius,
          bool along_columns) {
    const int step = along_columns ? c.w : 1;
    for (int y = 0; y < c.h; ++y) {
        for (int x = 0; x < c.w; ++x) {
            const int p = y * c.w + x;
            if (!c.mask[p]) {
                Copy3(&dst[3 * p], &src[3 * p]);
                continue;
            }
            const int pos = along_columns ? y : x;
            const int len = along_columns ? c.h : c.w;
            float w = 0, r = 0, g = 0, b = 0;
            auto tap = [&](int q, float t) {
                w = w + t;
                r = r + t * src[3 * q];
                g = g + t * src[3 * q + 1];
                b = b + t * src[3 * q + 2];
            };
            const int back = std::min(radius, pos);
            for (int k = 0; k <= back && c.mask[p - k * step]; ++k) tap(p - k * step, lut[k]);
            const int fwd = std::min(radius, len - pos - 1);
            for (int k = 1; k <= fwd && c.mask[p + k * step]; ++k) tap(p + k * step, lut[k]);
            const float inv = w == 0.0f ? 0.0f : 1.0f / w;
            dst[3 * p] = inv * r;
            dst[3 * p + 1] = inv * g;
            dst[3 * p + 2] = inv * b;
        }
    }
}

// sub_1800014F0 / sub_180001EA0 (Legacy). Called per band exactly like the
// binary because the "previous tap colour" carry is reset per call only.
// Taps at coordinate 0 are skipped (the binary tests `> 0`), out-of-range taps
// are skipped without stopping, and if every visited tap repeats the previous
// tap's colour the source pixel is copied unchanged.
void LegacyPass(const Canvas &c, const float *src, float *dst, const float *lut, int radius,
                bool along_columns, int band_start, int band_count) {
    const int step = along_columns ? c.w : 1;
    const int len = along_columns ? c.h : c.w;
    float prev[3] = {-1.0f, -1.0f, -1.0f};
    const int y0 = along_columns ? 0 : band_start;
    const int y1 = along_columns ? c.h : band_start + band_count;
    const int x0 = along_columns ? band_start : 0;
    const int x1 = along_columns ? band_start + band_count : c.w;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const int p = y * c.w + x;
            if (!c.mask[p]) {
                Copy3(&dst[3 * p], &src[3 * p]);
                continue;
            }
            const int pos = along_columns ? y : x;
            float w = 0, r = 0, g = 0, b = 0;
            bool same = true;
            // Returns false when a masked-out tap ends this direction.
            auto tap = [&](int k) {
                const int at = pos + k;
                if (at <= 0 || at >= len) return true;
                const int q = p + k * step;
                if (!c.mask[q]) return false;
                const float t = lut[radius + k];
                const float *s = &src[3 * q];
                w = w + t;
                r = r + s[0] * t;
                g = g + s[1] * t;
                b = b + s[2] * t;
                if (s[0] != prev[0] || s[1] != prev[1] || s[2] != prev[2]) same = false;
                Copy3(prev, s);
                return true;
            };
            for (int k = 0; k >= -radius && tap(k); --k) {}
            for (int k = 1; k <= radius && tap(k); ++k) {}
            if (same) {
                Copy3(&dst[3 * p], &src[3 * p]);
            } else {
                const float inv = w != 0.0f ? 1.0f / w : 1.0f;
                dst[3 * p] = inv * r;
                dst[3 * p + 1] = inv * g;
                dst[3 * p + 2] = inv * b;
            }
        }
    }
}

// The binary splits each pass into six bands: five of trunc(n/6.0) lines and
// a remainder band.
template <typename Fn>
void ForBands(int n, Fn fn) {
    const int band = (int)(float)((double)(float)n / 6.0);
    for (int i = 0; i < 5; ++i) fn(i * band, band);
    fn(5 * band, n - 5 * band);
}

void RunCurrent(Canvas &c, const OLMBlurParams &p, float scale) {
    const float amount = p.amount * scale;
    float k = 1.0f;
    if (p.repeat >= 2) k = powf(3.0f / amount, 1.0f / (float)(p.repeat - 1));
    if (p.bias != OLMBLUR_BIAS_VERTICAL && p.bias != OLMBLUR_BIAS_HORIZONTAL) return;
    std::vector<float> lut((size_t)std::max(0, (int)amount) + 2);
    for (int i = 0; i < p.repeat; ++i) {
        const float r = (float)((double)amount * pow((double)k, (double)i));
        const int radius = (int)r;
        if (radius == 0) break;
        // The binary sizes the LUT (int)amount+2 and overruns it when k > 1
        // (amount < 3, e.g. at reduced resolution); the port grows it.
        if ((size_t)radius >= lut.size()) lut.resize((size_t)radius + 1);
        const float s = r / 3.0f;
        const float denom = (s + s) * s;
        for (int j = 0; j <= radius; ++j) lut[j] = expf(-(float)(j * j) / denom);
        const bool columns_first = p.bias == OLMBLUR_BIAS_HORIZONTAL;
        Pass(c, c.a.data(), c.b.data(), lut.data(), radius, columns_first);
        Pass(c, c.b.data(), c.a.data(), lut.data(), radius, !columns_first);
    }
}

void RunLegacy(Canvas &c, const OLMBlurParams &p, float scale) {
    const int radius = (int)(p.amount * scale);
    const float base = (float)((p.amount * p.smoothness) / 100.0f) * (p.amount / 3.0f);
    const float sigma0 = base * scale;
    if (p.bias != OLMBLUR_BIAS_VERTICAL && p.bias != OLMBLUR_BIAS_HORIZONTAL) return;
    if (radius < 0) return;
    std::vector<float> lut((size_t)(2 * radius + 1));
    for (int i = 1; i <= p.repeat; ++i) {
        // sub_180009E10: symmetric LUT centred at `radius`.
        const float sigma = sigma0 / (float)i;
        const float denom = (sigma + sigma) * sigma;
        lut[radius] = 1.0f;
        for (int j = 1; j <= radius; ++j)
            lut[radius + j] = lut[radius - j] = expf(-(float)(j * j) / denom);
        const bool columns_first = p.bias == OLMBLUR_BIAS_HORIZONTAL;
        for (int pass = 0; pass < 2; ++pass) {
            const bool cols = (pass == 0) == columns_first;
            const float *src = pass == 0 ? c.a.data() : c.b.data();
            float *dst = pass == 0 ? c.b.data() : c.a.data();
            ForBands(cols ? c.w : c.h, [&](int start, int count) {
                LegacyPass(c, src, dst, lut.data(), radius, cols, start, count);
            });
        }
    }
}

// --- Pixel I/O: channels are read raw (0..255 / 0..32768 / float) ----------

inline float Raw(A_u_char v) { return (float)v; }
inline float Raw(A_u_short v) { return (float)v; }
inline float Raw(PF_FpShort v) { return v; }
inline void Store(A_u_char &d, float v) { d = (A_u_char)(int)floorf(v + 0.5f); }
inline void Store(A_u_short &d, float v) { d = (A_u_short)(int)floorf(v + 0.5f); }
inline void Store(PF_FpShort &d, float v) { d = v; }

template <typename Pixel>
Pixel *At(PF_EffectWorld *w, int x, int y) {
    return (Pixel *)((char *)w->data + (ptrdiff_t)y * w->rowbytes) + x;
}

template <typename Pixel>
void Kernel(PF_InData *in_data, PF_EffectWorld *input, PF_EffectWorld *output,
            const OLMBlurParams &p) {
    Canvas c;
    c.w = input->width;
    c.h = input->height;
    if (c.w <= 0 || c.h <= 0) return;
    const size_t n = (size_t)c.w * (size_t)c.h;
    c.mask.resize(n);
    c.a.resize(3 * n);
    c.b.resize(3 * n);
    for (int y = 0; y < c.h; ++y) {
        for (int x = 0; x < c.w; ++x) {
            const Pixel &px = *At<Pixel>(input, x, y);
            float *d = &c.a[3 * ((size_t)y * c.w + x)];
            d[0] = Raw(px.red);
            d[1] = Raw(px.green);
            d[2] = Raw(px.blue);
            c.mask[(size_t)y * c.w + x] = px.alpha != 0;
        }
    }

    const float scale = (float)in_data->downsample_x.num / (float)in_data->downsample_x.den;
    if (p.legacy) {
        RunLegacy(c, p, scale);
    } else {
        if (p.amount == 0.0f) return;  // output already holds the input copy
        RunCurrent(c, p, scale);
    }

    // Alpha keeps the utils->copy value; only RGB is written.
    const int w = std::min(c.w, (int)output->width);
    const int h = std::min(c.h, (int)output->height);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            Pixel &px = *At<Pixel>(output, x, y);
            const float *s = &c.a[3 * ((size_t)y * c.w + x)];
            Store(px.red, s[0]);
            Store(px.green, s[1]);
            Store(px.blue, s[2]);
        }
    }
}

PF_Err SmartRender(PF_InData *in_data, PF_SmartRenderExtra *extra) {
    PF_EffectWorld *input = nullptr;
    PF_EffectWorld *output = nullptr;
    PF_Err err = extra->cb->checkout_layer_pixels(in_data->effect_ref, OLMBLUR_INPUT, &input);
    const bool checked_out = !err;
    if (!err) err = extra->cb->checkout_output(in_data->effect_ref, &output);
    if (!err && (!input || !output)) err = PF_Err_BAD_CALLBACK_PARAM;

    try {
        // Defaults the binary seeds before checkout (0x18000A380).
        OLMBlurParams params{5.0f, 100.0f, 1, OLMBLUR_BIAS_VERTICAL, false};
        const int depth = extra->input->bitdepth;
        if (!err && (depth == 8 || depth == 16 || depth == 32)) {
            err = CheckoutParams(in_data, &params);
            if (!err)
                err = in_data->utils->copy(in_data->effect_ref, input, output, nullptr, nullptr);
            if (!err) {
                if (depth == 8) Kernel<PF_Pixel>(in_data, input, output, params);
                else if (depth == 16) Kernel<PF_Pixel16>(in_data, input, output, params);
                else Kernel<PF_PixelFloat>(in_data, input, output, params);
            }
        }
    } catch (...) {
        if (checked_out) extra->cb->checkin_layer_pixels(in_data->effect_ref, OLMBLUR_INPUT);
        throw;
    }
    if (checked_out) {
        const PF_Err checkin_err =
            extra->cb->checkin_layer_pixels(in_data->effect_ref, OLMBLUR_INPUT);
        if (!err) err = checkin_err;
    }
    return err;
}

}  // namespace

PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
                  PF_ParamDef *params[], PF_LayerDef *output, void *extra) {
    (void)params;
    (void)output;
    try {
        switch (cmd) {
            case PF_Cmd_ABOUT:
                return About(in_data, out_data);
            case PF_Cmd_GLOBAL_SETUP:
                return GlobalSetup(in_data, out_data);
            case PF_Cmd_PARAMS_SETUP:
                return ParamsSetup(in_data, out_data);
            case PF_Cmd_UPDATE_PARAMS_UI:
                return UpdateParamsUI(in_data);
            case PF_Cmd_SMART_PRE_RENDER:
                return PreRender(in_data, (PF_PreRenderExtra *)extra);
            case PF_Cmd_SMART_RENDER:
                return SmartRender(in_data, (PF_SmartRenderExtra *)extra);
            case PF_Cmd_RENDER:  // binary's legacy path misreads float params; unreachable under SmartRender
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

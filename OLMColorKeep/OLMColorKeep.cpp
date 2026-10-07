/* OLMColorKeep: macOS SmartRender port of the Windows OLM Color Keep 1.0.1
 * (ColorKeep.aex). Follows the decompiled binary: entry 0x1800025C0,
 * UI 0x180002210 + per-stream helper 0x180001FF0, param checkout 0x180001A90,
 * smart render 0x180001CA0, 8/16/32 kernels 0x180001140/1000 + float inline,
 * pixel functions 0x180001580/1280/1850, rect union 0x180003B80. */

#include "OLMColorKeep.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace {

static_assert(PF_VERSION(OLMCKP_MAJOR_VERSION, OLMCKP_MINOR_VERSION, OLMCKP_BUG_VERSION,
                         PF_Stage_DEVELOP, 0) == 526336,
              "ColorKeep version must match its PiPL");

AEGP_PluginID g_aegp_id = 0;

PF_Err About(PF_InData *in_data, PF_OutData *out_data) {
    PF_SPRINTF(out_data->return_msg, "%s", OLMCKP_ABOUT);
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData *in_data, PF_OutData *out_data) {
    out_data->my_version = PF_VERSION(OLMCKP_MAJOR_VERSION, OLMCKP_MINOR_VERSION,
                                      OLMCKP_BUG_VERSION, PF_Stage_DEVELOP, 0);
    // Binary: 0x02000040. The port adds SEND_UPDATE_PARAMS_UI so a freshly
    // applied effect hides the unused Color pickers immediately (the binary
    // only reacts to USER_CHANGED_PARAM via SUPERVISE on the count slider).
    out_data->out_flags = PF_OutFlag_USE_OUTPUT_EXTENT | PF_OutFlag_DEEP_COLOR_AWARE |
                          PF_OutFlag_SEND_UPDATE_PARAMS_UI;
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER |
                           PF_OutFlag2_FLOAT_COLOR_AWARE |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    if (in_data->pica_basicP) {
        AEGP_SuiteHandler suites(in_data->pica_basicP);
        suites.UtilitySuite3()->AEGP_RegisterWithAEGP(nullptr, OLMCKP_NAME, &g_aegp_id);
    }
    return PF_Err_NONE;
}

PF_Err ParamsSetup(PF_InData *in_data, PF_OutData *out_data) {
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_SUPERVISE;
    PF_ADD_SLIDER("Enabled Color Num", 0, OLMCKP_MAX_COLORS, 0, OLMCKP_MAX_COLORS, 1,
                  OLMCKP_COUNT);

    for (int i = 0; i < OLMCKP_MAX_COLORS; ++i) {
        AEFX_CLR_STRUCT(def);
        PF_ADD_COLOR("Color", 0, 0, 0, OLMCKP_COLOR_BASE + i);
    }
    out_data->num_params = OLMCKP_NUM_PARAMS;
    return PF_Err_NONE;
}

// --- UI: hide Color n for n >= count ----------------------------------------

PF_Err UpdateParamsUI(PF_InData *in_data, PF_ParamDef *params[]) {
    if (!in_data->pica_basicP || !g_aegp_id || !params || !params[OLMCKP_COUNT])
        return PF_Err_NONE;
    const A_long count = params[OLMCKP_COUNT]->u.sd.value;

    AEGP_SuiteHandler suites(in_data->pica_basicP);
    AEGP_LayerH layerH = nullptr;
    PF_Err err = suites.PFInterfaceSuite1()->AEGP_GetEffectLayer(in_data->effect_ref, &layerH);
    if (err || !layerH) return err;
    AEGP_EffectRefH effectH = nullptr;
    err = suites.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(g_aegp_id, in_data->effect_ref,
                                                                 &effectH);
    if (err || !effectH) return err;

    // sub_180001FF0 per stream; the binary ignores per-stream errors.
    for (int i = 0; i < OLMCKP_MAX_COLORS; ++i) {
        AEGP_StreamRefH streamH = nullptr;
        if (suites.StreamSuite7()->AEGP_GetNewEffectStreamByIndex(
                g_aegp_id, effectH, OLMCKP_COLOR_BASE + i, &streamH) || !streamH)
            continue;
        suites.DynamicStreamSuite3()->AEGP_SetDynamicStreamFlag(
            streamH, AEGP_DynStreamFlag_HIDDEN, FALSE, i >= count ? TRUE : FALSE);
        suites.StreamSuite7()->AEGP_DisposeStream(streamH);
    }
    return suites.EffectSuite5()->AEGP_DisposeEffect(effectH);
}

// --- PreRender ----------------------------------------------------------------

PF_Err PreRender(PF_InData *in_data, PF_PreRenderExtra *extra) {
    PF_CheckoutResult result;
    AEFX_CLR_STRUCT(result);
    PF_RenderRequest request = extra->input->output_request;
    PF_Err err = extra->cb->checkout_layer(in_data->effect_ref, OLMCKP_INPUT, 0, &request,
                                           in_data->current_time, in_data->time_step,
                                           in_data->time_scale, &result);
    if (!err) {
        UnionLRect(&result.result_rect, &extra->output->result_rect);
        UnionLRect(&result.max_result_rect, &extra->output->max_result_rect);
    }
    return err;
}

// --- Params checkout (0x180001A90) -------------------------------------------

PF_Err CheckoutParams(PF_InData *in_data, PF_OutData *out_data, OLMCKPParams *p) {
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    PF_Err err = PF_CHECKOUT_PARAM(in_data, OLMCKP_COUNT, in_data->current_time,
                                   in_data->time_step, in_data->time_scale, &def);
    if (err) return err;
    // Guard: the binary indexes its 100-entry array with the raw value.
    p->count = std::min<A_long>(std::max<A_long>(def.u.sd.value, 0), OLMCKP_MAX_COLORS);
    err = PF_CHECKIN_PARAM(in_data, &def);

    AEFX_SuiteScoper<PF_ColorParamSuite1> color_suite(in_data, kPFColorParamSuite,
                                                      kPFColorParamSuiteVersion1, out_data);
    for (int i = 0; i < OLMCKP_MAX_COLORS && !err; ++i) {
        AEFX_CLR_STRUCT(def);
        err = PF_CHECKOUT_PARAM(in_data, OLMCKP_COLOR_BASE + i, in_data->current_time,
                                in_data->time_step, in_data->time_scale, &def);
        if (err) break;
        err = color_suite->PF_GetFloatingPointColorFromColorDef(in_data->effect_ref, &def,
                                                                &p->colors[i]);
        const PF_Err checkin_err = PF_CHECKIN_PARAM(in_data, &def);
        if (!err) err = checkin_err;
    }
    return err;
}

// --- Pixel functions ----------------------------------------------------------
// A pixel is kept when ALL FOUR channels (alpha included) equal a key color
// quantized with rounding: (int)((c + 1/(2*max)) * max). Key colors come from
// color params, so alpha is 1.0: only fully opaque pixels can match.

inline bool Matches(const PF_Pixel &in, const PF_PixelFloat &c) {
    auto q = [](float v) { return (A_u_char)(int)((v + 0.0019607844f) * 255.0f); };
    return in.alpha == q(c.alpha) && in.red == q(c.red) && in.green == q(c.green) &&
           in.blue == q(c.blue);
}

inline bool Matches(const PF_Pixel16 &in, const PF_PixelFloat &c) {
    auto q = [](float v) { return (A_u_short)(int)((v + 0.000015258789f) * 32768.0f); };
    return in.alpha == q(c.alpha) && in.red == q(c.red) && in.green == q(c.green) &&
           in.blue == q(c.blue);
}

inline bool Matches(const PF_PixelFloat &in, const PF_PixelFloat &c) {
    // comiss/ja in the binary: a NaN difference does not reject the match.
    auto near = [](float a, float b) { return !(std::fabs(a - b) > 0.0001f); };
    return near(in.alpha, c.alpha) && near(in.red, c.red) && near(in.green, c.green) &&
           near(in.blue, c.blue);
}

template <typename Pixel>
PF_Err KeepPixel(void *refcon, A_long, A_long, Pixel *in, Pixel *out) {
    const OLMCKPParams *p = static_cast<const OLMCKPParams *>(refcon);
    bool hit = false;
    for (A_long i = 0; i < p->count && !hit; ++i) hit = Matches(*in, p->colors[i]);
    out->red = in->red;
    out->green = in->green;
    out->blue = in->blue;
    out->alpha = hit ? in->alpha : 0;
    return PF_Err_NONE;
}

template <typename Suite, typename Pixel>
PF_Err Kernel(PF_InData *in_data, PF_OutData *out_data, const char *suite_name,
              int suite_version, PF_EffectWorld *input, PF_EffectWorld *output,
              OLMCKPParams *p) {
    PF_Err err = in_data->utils->copy(in_data->effect_ref, input, output, nullptr, nullptr);
    if (err) return err;
    AEFX_SuiteScoper<Suite> iterate_suite(in_data, suite_name, suite_version, out_data);
    const PF_Rect *area = &output->extent_hint;
    return iterate_suite->iterate(in_data, 0, area->bottom - area->top, input, area, p,
                                  KeepPixel<Pixel>, output);
}

PF_Err SmartRender(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra) {
    PF_EffectWorld *input = nullptr;
    PF_EffectWorld *output = nullptr;
    PF_Err err = extra->cb->checkout_layer_pixels(in_data->effect_ref, OLMCKP_INPUT, &input);
    const bool checked_out = !err;
    if (!err) err = extra->cb->checkout_output(in_data->effect_ref, &output);
    if (!err && (!input || !output)) err = PF_Err_BAD_CALLBACK_PARAM;

    try {
        OLMCKPParams params{};
        if (!err) err = CheckoutParams(in_data, out_data, &params);
        if (!err) {
            switch (extra->input->bitdepth) {
                case 8:
                    err = Kernel<PF_Iterate8Suite1, PF_Pixel>(
                        in_data, out_data, kPFIterate8Suite, kPFIterate8SuiteVersion1, input,
                        output, &params);
                    break;
                case 16:
                    err = Kernel<PF_Iterate16Suite1, PF_Pixel16>(
                        in_data, out_data, kPFIterate16Suite, kPFIterate16SuiteVersion1, input,
                        output, &params);
                    break;
                case 32:
                    err = Kernel<PF_IterateFloatSuite1, PF_PixelFloat>(
                        in_data, out_data, kPFIterateFloatSuite, kPFIterateFloatSuiteVersion1,
                        input, output, &params);
                    break;
                default:
                    break;  // binary: unknown depth is a silent no-op
            }
        }
    } catch (...) {
        if (checked_out) extra->cb->checkin_layer_pixels(in_data->effect_ref, OLMCKP_INPUT);
        throw;
    }
    // The original never checks in (host reclaims); balanced here like ColorKey.
    if (checked_out) {
        const PF_Err checkin_err =
            extra->cb->checkin_layer_pixels(in_data->effect_ref, OLMCKP_INPUT);
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
                return GlobalSetup(in_data, out_data);
            case PF_Cmd_PARAMS_SETUP:
                return ParamsSetup(in_data, out_data);
            case PF_Cmd_USER_CHANGED_PARAM:
            case PF_Cmd_UPDATE_PARAMS_UI:
                return UpdateParamsUI(in_data, params);
            case PF_Cmd_SMART_PRE_RENDER:
                return PreRender(in_data, (PF_PreRenderExtra *)extra);
            case PF_Cmd_SMART_RENDER:
                return SmartRender(in_data, out_data, (PF_SmartRenderExtra *)extra);
            case PF_Cmd_RENDER:  // binary's legacy path reads uninitialized params; unreachable under SmartRender
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

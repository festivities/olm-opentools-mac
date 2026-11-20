/* OLMColorKeep: macOS port of the Windows OLM Color Keep 1.1.0
 * (ColorKeep-20261120.aex). Behaviour follows the verified binary (spec:
 * %TEMP%\opencode\specs\OLMColorKeep-20261120.md):
 *   entry_point 0x180006690 (MyEffect dispatch), PARAMS_SETUP 0x180001000,
 *   UPDATE_PARAMS_UI 0x180001460, SMART_PRE_RENDER 0x180001BC0,
 *   SMART_RENDER 0x1800010D0, pixel kernels 0x1800015C0 / 0x180001690 / 0x180001780.
 * Every other command is a no-op returning PF_Err_NONE, as in the binary.
 */

#include "OLMColorKeep.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace {

static_assert(PF_VERSION(OLMCKP_MAJOR_VERSION, OLMCKP_MINOR_VERSION, OLMCKP_BUG_VERSION,
                         PF_Stage_DEVELOP, 0) == 557056,
              "OLM Color Keep version must match its PiPL (0x00088000)");

// Float constants by bit pattern, exactly as the binary loads them.
inline float BitsToFloat(unsigned int bits) {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
const float kScale8 = BitsToFloat(0x3B808081u);   // 1/255 as float (sub_180007140)
const float kScale16 = BitsToFloat(0x38000000u);  // 2^-15 (sub_1800071B0)
const float kEpsilon = BitsToFloat(0x38D1B717u);  // 1e-4f (kernels)

AEGP_PluginID g_aegp_id = 0;

PF_Err About(PF_InData *in_data, PF_OutData *out_data) {
    PF_SPRINTF(out_data->return_msg, "%s %d.%d\r%s", OLMCKP_NAME, OLMCKP_MAJOR_VERSION,
               OLMCKP_MINOR_VERSION, OLMCKP_ABOUT_DESC);
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData *in_data, PF_OutData *out_data) {
    out_data->my_version = PF_VERSION(OLMCKP_MAJOR_VERSION, OLMCKP_MINOR_VERSION,
                                      OLMCKP_BUG_VERSION, PF_Stage_DEVELOP, 0);
    // Binary is 0x02000040. Deliberate deviation: add SEND_UPDATE_PARAMS_UI
    // (0x06000040) so AE delivers UPDATE_PARAMS_UI and unused pickers are hidden
    // (same choice as the 1.0.1 port and the ColorKey/Smoother/KiraKira ports).
    out_data->out_flags = PF_OutFlag_USE_OUTPUT_EXTENT | PF_OutFlag_DEEP_COLOR_AWARE |
                          PF_OutFlag_SEND_UPDATE_PARAMS_UI;
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER |
                           PF_OutFlag2_FLOAT_COLOR_AWARE |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    if (in_data->pica_basicP) {
        AEGP_SuiteHandler suites(in_data->pica_basicP);
        suites.UtilitySuite6()->AEGP_RegisterWithAEGP(nullptr, OLMCKP_NAME, &g_aegp_id);
    }
    return PF_Err_NONE;
}

PF_Err AddParam(PF_InData *in_data, PF_ParamDef *def) {
    return in_data->inter.add_param(in_data->effect_ref, -1, def);
}

PF_Err ParamsSetup(PF_InData *in_data, PF_OutData *out_data) {
    // Enabled Color Num: SLIDER, valid and slider range 0..200, default 1, SUPERVISE.
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    def.uu.id = OLMCKP_COUNT;
    def.param_type = PF_Param_SLIDER;
    def.flags = PF_ParamFlag_SUPERVISE;
    PF_STRCPY(def.PF_DEF_NAME, "Enabled Color Num");
    def.u.sd.value = 1;
    def.u.sd.valid_min = 0;
    def.u.sd.valid_max = OLMCKP_MAX_COLORS;
    def.u.sd.slider_min = 0;
    def.u.sd.slider_max = OLMCKP_MAX_COLORS;
    def.u.sd.dephault = 1;
    PF_Err err = AddParam(in_data, &def);
    if (err) return err;

    // Color 1..200: COLOR, opaque black (A=255), no flags.
    for (int i = 0; i < OLMCKP_MAX_COLORS && !err; ++i) {
        AEFX_CLR_STRUCT(def);
        def.uu.id = OLMCKP_COLOR_BASE + i;
        def.param_type = PF_Param_COLOR;
        PF_STRCPY(def.PF_DEF_NAME, "Color");
        def.u.cd.value.alpha = 255;
        def.u.cd.dephault = def.u.cd.value;
        err = AddParam(in_data, &def);
    }
    if (!err) out_data->num_params = OLMCKP_NUM_PARAMS;
    return err;
}

// --- UI (UPDATE_PARAMS_UI 0x180001460) ----------------------------------------

// sub_180002530: HIDDEN stream flag on the color stream with index `id`.
PF_Err SetColorHidden(PF_InData *in_data, A_long id, A_Boolean hidden) {
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    AEGP_EffectRefH effectH = nullptr;
    PF_Err err = suites.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(
        g_aegp_id, in_data->effect_ref, &effectH);
    if (err) return err;
    AEGP_StreamRefH streamH = nullptr;
    err = suites.StreamSuite5()->AEGP_GetNewEffectStreamByIndex(g_aegp_id, effectH, id, &streamH);
    if (!err)
        err = suites.DynamicStreamSuite4()->AEGP_SetDynamicStreamFlag(
            streamH, AEGP_DynStreamFlag_HIDDEN, FALSE, hidden);
    if (streamH) suites.StreamSuite5()->AEGP_DisposeStream(streamH);
    const PF_Err dispose_err = suites.EffectSuite4()->AEGP_DisposeEffect(effectH);
    return err ? err : dispose_err;
}

PF_Err UpdateParamsUI(PF_InData *in_data) {
    if (!in_data->pica_basicP) return PF_Err_NONE;
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    PF_Err err = PF_CHECKOUT_PARAM(in_data, OLMCKP_COUNT, in_data->current_time,
                                   in_data->time_step, in_data->time_scale, &def);
    if (err) return err;
    const A_long count = def.u.sd.value;  // raw, unclamped (binary)
    err = PF_CHECKIN_PARAM(in_data, &def);
    if (err) return err;

    // Colours 2..201: hidden when index >= count. Binary order and per-id AEGP calls.
    for (A_long i = 0; i < OLMCKP_MAX_COLORS && !err; ++i)
        err = SetColorHidden(in_data, OLMCKP_COLOR_BASE + i, i >= count ? TRUE : FALSE);
    return err;
}

// --- PreRender (0x180001BC0) --------------------------------------------------

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

// --- SmartRender (0x1800010D0) ------------------------------------------------

// Count and colours at the current time. The binary reads count, then colours
// 2..count+1 in order (sub_180004340 / sub_180003B80 -> ColorParamSuite1).
PF_Err CheckoutParams(PF_InData *in_data, PF_OutData *out_data, OLMCKPParams *p) {
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    PF_Err err = PF_CHECKOUT_PARAM(in_data, OLMCKP_COUNT, in_data->current_time,
                                   in_data->time_step, in_data->time_scale, &def);
    if (err) return err;
    // Port guard: the binary overruns its colour buffer for count > 200.
    p->count = std::min<A_long>(std::max<A_long>(def.u.sd.value, 0), OLMCKP_MAX_COLORS);
    err = PF_CHECKIN_PARAM(in_data, &def);
    if (err) return err;

    AEFX_SuiteScoper<PF_ColorParamSuite1> color_suite(in_data, kPFColorParamSuite,
                                                      kPFColorParamSuiteVersion1, out_data);
    for (A_long i = 0; i < p->count && !err; ++i) {
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

// Channel values as the binary's converters produce them (R, G, B only; alpha is
// not compared). 8 bpc: byte * 1/255f; 16 bpc: word * 2^-15; 32 bpc: raw float.
inline void ToFloat(const PF_Pixel8 &p, float *r, float *g, float *b) {
    *r = (float)p.red * kScale8;
    *g = (float)p.green * kScale8;
    *b = (float)p.blue * kScale8;
}
inline void ToFloat(const PF_Pixel16 &p, float *r, float *g, float *b) {
    *r = (float)p.red * kScale16;
    *g = (float)p.green * kScale16;
    *b = (float)p.blue * kScale16;
}
inline void ToFloat(const PF_PixelFloat &p, float *r, float *g, float *b) {
    *r = p.red;
    *g = p.green;
    *b = p.blue;
}

// Mismatch when |colour - pixel| > 1e-4f on any channel. NaN compares as a
// match (the binary's `fabs(...) > eps` is false for NaN).
inline bool MatchesColor(const PF_PixelFloat &c, float r, float g, float b) {
    return !(std::fabs(c.red - r) > kEpsilon) && !(std::fabs(c.green - g) > kEpsilon) &&
           !(std::fabs(c.blue - b) > kEpsilon);
}

template <typename Pixel>
PF_Err KeepPixel(void *refcon, A_long, A_long, Pixel *in, Pixel *out) {
    const OLMCKPParams *p = static_cast<const OLMCKPParams *>(refcon);
    float r = 0.0f, g = 0.0f, b = 0.0f;
    ToFloat(*in, &r, &g, &b);
    bool hit = false;
    for (A_long i = 0; i < p->count && !hit; ++i) hit = MatchesColor(p->colors[i], r, g, b);
    // Binary writes only the first channel (alpha): RGB come from utils->copy.
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
    const A_long progress_final = std::max<A_long>(0, area->bottom - area->top + 1);
    return iterate_suite->iterate(in_data, 0, progress_final, input, area, p,
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
                    err = Kernel<PF_Iterate8Suite1, PF_Pixel8>(
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
                    break;  // binary returns count as an error here; port is a silent no-op
            }
        }
    } catch (...) {
        if (checked_out) extra->cb->checkin_layer_pixels(in_data->effect_ref, OLMCKP_INPUT);
        throw;
    }
    // Deviation: the binary never checks the input back in. Balanced here.
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
                return SmartRender(in_data, out_data, (PF_SmartRenderExtra *)extra);
            // USER_CHANGED_PARAM, EVENT, ARBITRARY, SEQUENCE_*, legacy RENDER and
            // GLOBAL_SETDOWN are no-ops in the binary (GLOBAL_SETDOWN only acquires
            // and releases AEGP Compute Cache, which this port does not use).
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

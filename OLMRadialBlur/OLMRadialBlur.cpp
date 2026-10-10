/* OLMRadialBlur: macOS SmartRender port of the Windows OLM RadialBlur 1.3.0.
 * The per-pixel work follows the decompiled 8/16/32-bpc float kernels. */

#include "OLMRadialBlur.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegreesToRadians = 0.0174532925f;
constexpr double kFixedDegreesToRadians = 0.0000002663161090079238;
static_assert(PF_VERSION(1, 3, 0, PF_Stage_DEVELOP, 0) == 622592,
              "RadialBlur version must match its PiPL");

AEGP_PluginID g_aegp_id = 0;

PF_Err About(PF_InData *in_data, PF_OutData *out_data) {
    (void)in_data;
    PF_SPRINTF(out_data->return_msg, "%s", OLMRB_ABOUT);
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData *in_data, PF_OutData *out_data) {
    out_data->my_version = PF_VERSION(OLMRB_MAJOR_VERSION, OLMRB_MINOR_VERSION,
                                      OLMRB_BUG_VERSION, PF_Stage_DEVELOP, 0);
    out_data->out_flags = PF_OutFlag_USE_OUTPUT_EXTENT | PF_OutFlag_DEEP_COLOR_AWARE |
                          PF_OutFlag_SEND_UPDATE_PARAMS_UI | PF_OutFlag_CUSTOM_UI;
    out_data->out_flags2 = PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG |
                           PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    if (in_data->pica_basicP) {
        AEGP_SuiteHandler suites(in_data->pica_basicP);
        suites.UtilitySuite3()->AEGP_RegisterWithAEGP((AEGP_GlobalRefcon)0, OLMRB_NAME,
                                                      &g_aegp_id);
    }
    return PF_Err_NONE;
}

// Windows 1.3.0 PARAMS_SETUP values (flags / ui_flags arguments of the binary's param helpers):
// popup, checkbox and layer params 0x62; point and angle 0x60; float sliders SUPERVISE;
// integer sliders flags 1 with PF_PUI_DONT_ERASE_TOPIC (0x40).
#define OLMRB_CHOICE_FLAGS (PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_COLLAPSE_TWIRLY | PF_ParamFlag_SUPERVISE)

PF_Err ParamsSetup(PF_InData *in_data, PF_OutData *out_data) {
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Blur Type", 2, OLMRB_BLUR_ZOOM, "Zoom | Rotation",
                  OLMRB_CHOICE_FLAGS, OLMRB_ID_BLUR_TYPE);

    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_COLLAPSE_TWIRLY | PF_ParamFlag_SUPERVISE;
    PF_ADD_POINT("Center", 50, 50, FALSE, OLMRB_ID_CENTER);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Outer Blur", PF_ParamFlag_COLLAPSE_TWIRLY, OLMRB_ID_OUTER_GROUP);

    AEFX_CLR_STRUCT(def);
    def.flags = 1;
    def.ui_flags = PF_PUI_DONT_ERASE_TOPIC;
    PF_ADD_SLIDER("Strength", 0, 2000, 0, 2000, 0, OLMRB_ID_OUTER_STRENGTH);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Offset Mode", 3, OLMRB_OFFSET_ADD, "Add | Max | Override",
                  OLMRB_CHOICE_FLAGS, OLMRB_ID_OUTER_OFFSET_MODE);

    AEFX_CLR_STRUCT(def);
    def.flags = 1;
    def.ui_flags = PF_PUI_DONT_ERASE_TOPIC;
    PF_ADD_SLIDER("Offset", 0, 500, 0, 500, 0, OLMRB_ID_OUTER_OFFSET);

    AEFX_CLR_STRUCT(def);
    def.flags = 1;
    def.ui_flags = PF_PUI_DONT_ERASE_TOPIC;
    PF_ADD_SLIDER("Edge Fade", 0, 100, 0, 100, 0, OLMRB_ID_OUTER_FADE);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OLMRB_ID_OUTER_GROUP_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Inner Blur", PF_ParamFlag_COLLAPSE_TWIRLY, OLMRB_ID_INNER_GROUP);

    AEFX_CLR_STRUCT(def);
    def.flags = 1;
    def.ui_flags = PF_PUI_DONT_ERASE_TOPIC;
    PF_ADD_SLIDER("Strength", 0, 2000, 0, 2000, 0, OLMRB_ID_INNER_STRENGTH);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Offset Mode", 3, OLMRB_OFFSET_ADD, "Add | Max | Override",
                  OLMRB_CHOICE_FLAGS, OLMRB_ID_INNER_OFFSET_MODE);

    AEFX_CLR_STRUCT(def);
    def.flags = 1;
    def.ui_flags = PF_PUI_DONT_ERASE_TOPIC;
    PF_ADD_SLIDER("Offset", 0, 500, 0, 500, 0, OLMRB_ID_INNER_OFFSET);

    AEFX_CLR_STRUCT(def);
    def.flags = 1;
    def.ui_flags = PF_PUI_DONT_ERASE_TOPIC;
    PF_ADD_SLIDER("Edge Fade", 0, 100, 0, 100, 0, OLMRB_ID_INNER_FADE);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OLMRB_ID_INNER_GROUP_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Repeat Border", 1, OLMRB_CHOICE_FLAGS, OLMRB_ID_REPEAT_BORDER);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Ellipse", PF_ParamFlag_COLLAPSE_TWIRLY, OLMRB_ID_ELLIPSE_GROUP);

    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_SUPERVISE;
    PF_ADD_FLOAT_SLIDER("Ratio", 1, 5, 1, 5, AEFX_DEFAULT_CURVE_TOLERANCE, 1.0, 2,
                        PF_ValueDisplayFlag_NONE, false, OLMRB_ID_RATIO);

    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_COLLAPSE_TWIRLY | PF_ParamFlag_SUPERVISE;
    PF_ADD_ANGLE("Angle", 0, OLMRB_ID_ANGLE);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OLMRB_ID_ELLIPSE_GROUP_END);

    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_SUPERVISE;
    PF_ADD_FLOAT_SLIDER("Quality", 1, 50, 1, 50, AEFX_DEFAULT_CURVE_TOLERANCE, 5.0, 2,
                        PF_ValueDisplayFlag_NONE, false, OLMRB_ID_QUALITY);

    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_SUPERVISE;
    PF_ADD_FLOAT_SLIDER("Brightness Gain", 0, 10, 0, 2, AEFX_DEFAULT_CURVE_TOLERANCE, 1.0, 1,
                        PF_ValueDisplayFlag_NONE, false, OLMRB_ID_BRIGHTNESS);

    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_SUPERVISE;
    PF_ADD_FLOAT_SLIDER("Size Variation", 0, 100, 0, 100, AEFX_DEFAULT_CURVE_TOLERANCE, 0.0, 1,
                        PF_ValueDisplayFlag_NONE, false, OLMRB_ID_SIZE_VARIATION);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Noise Parameters", PF_ParamFlag_COLLAPSE_TWIRLY, OLMRB_ID_NOISE_GROUP);

    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_SUPERVISE;
    PF_ADD_FLOAT_SLIDER("Noise Variation", 0, 100, 0, 100, AEFX_DEFAULT_CURVE_TOLERANCE, 0.0, 1,
                        PF_ValueDisplayFlag_NONE, false, OLMRB_ID_NOISE_VARIATION);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Noise Type", 3, OLMRB_NOISE_SMOOTH, "Smooth | Block | Layer",
                  OLMRB_CHOICE_FLAGS, OLMRB_ID_NOISE_TYPE);

    AEFX_CLR_STRUCT(def);
    def.flags = OLMRB_CHOICE_FLAGS;
    PF_ADD_LAYER("Noise Layer", 0, OLMRB_ID_NOISE_LAYER);

    AEFX_CLR_STRUCT(def);
    def.flags = 1;
    def.ui_flags = PF_PUI_DONT_ERASE_TOPIC;
    PF_ADD_SLIDER("Seed", 1, 1000, 1, 1000, 1, OLMRB_ID_SEED);

    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_COLLAPSE_TWIRLY | PF_ParamFlag_SUPERVISE;
    PF_ADD_ANGLE("Offset", 0, OLMRB_ID_NOISE_OFFSET);

    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_SUPERVISE;
    PF_ADD_FLOAT_SLIDER("Thickness", 1, 100, 1, 100, AEFX_DEFAULT_CURVE_TOLERANCE, 10.0, 2,
                        PF_ValueDisplayFlag_NONE, false, OLMRB_ID_THICKNESS);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OLMRB_ID_NOISE_GROUP_END);

    out_data->num_params = OLMRB_NUM_PARAMS;
    return PF_Err_NONE;
}

template <typename ReadValue>
PF_Err checkout_value(PF_InData *in_data, PF_ParamIndex position, ReadValue read_value) {
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    PF_Err err = PF_CHECKOUT_PARAM(in_data, position, in_data->current_time, in_data->time_step,
                                   in_data->time_scale, &def);
    if (err) return err;
    read_value(def);
    return PF_CHECKIN_PARAM(in_data, &def);
}

PF_Err CheckoutParams(PF_InData *in_data, PF_OutData *out_data, OLMRBParams *p) {
    memset(p, 0, sizeof(*p));
    PF_Err err;

#define OLMRB_READ(POS, BODY) \
    do { \
        err = checkout_value(in_data, (POS), [&](const PF_ParamDef &def) { BODY; }); \
        if (err) return err; \
    } while (0)

    OLMRB_READ(OLMRB_BLUR_TYPE, p->blur_type = def.u.pd.value);

    {
        PF_ParamDef point_def;
        AEFX_CLR_STRUCT(point_def);
        err = PF_CHECKOUT_PARAM(in_data, OLMRB_CENTER, in_data->current_time, in_data->time_step,
                                in_data->time_scale, &point_def);
        if (err) return err;
        A_FloatPoint point;
        AEFX_CLR_STRUCT(point);
        if (!in_data->pica_basicP) {
            PF_CHECKIN_PARAM(in_data, &point_def);
            return PF_Err_BAD_CALLBACK_PARAM;
        }
        try {
            AEFX_SuiteScoper<PF_PointParamSuite1> point_suite(
                in_data, kPFPointParamSuite, kPFPointParamSuiteVersion1, out_data);
            err = point_suite->PF_GetFloatingPointValueFromPointDef(
                in_data->effect_ref, &point_def, &point);
        } catch (...) {
            PF_CHECKIN_PARAM(in_data, &point_def);
            throw;
        }
        if (!err) {
            // PF_PointParamSuite returns layer-space pixels. Undo AE's render
            // downsample here; the kernel applies its float scale after origins.
            p->center_x = (double)point.x * (double)in_data->downsample_x.den /
                          (double)in_data->downsample_x.num;
            p->center_y = (double)point.y * (double)in_data->downsample_y.den /
                          (double)in_data->downsample_y.num;
        }
        PF_Err checkin_err = PF_CHECKIN_PARAM(in_data, &point_def);
        if (err) return err;
        if (checkin_err) return checkin_err;
    }

    OLMRB_READ(OLMRB_OUTER_STRENGTH, p->outer_strength = def.u.sd.value);
    OLMRB_READ(OLMRB_OUTER_OFFSET_MODE, p->outer_offset_mode = def.u.pd.value);
    OLMRB_READ(OLMRB_OUTER_OFFSET, p->outer_offset = def.u.sd.value);
    OLMRB_READ(OLMRB_OUTER_FADE, p->outer_fade = def.u.sd.value);
    OLMRB_READ(OLMRB_INNER_STRENGTH, p->inner_strength = def.u.sd.value);
    OLMRB_READ(OLMRB_INNER_OFFSET_MODE, p->inner_offset_mode = def.u.pd.value);
    OLMRB_READ(OLMRB_INNER_OFFSET, p->inner_offset = def.u.sd.value);
    OLMRB_READ(OLMRB_INNER_FADE, p->inner_fade = def.u.sd.value);
    OLMRB_READ(OLMRB_REPEAT_BORDER, p->repeat_border = def.u.bd.value != 0);
    OLMRB_READ(OLMRB_RATIO, p->ratio = (float)def.u.fs_d.value);
    OLMRB_READ(OLMRB_ANGLE, p->angle_rad = (float)((double)def.u.ad.value * kFixedDegreesToRadians));
    OLMRB_READ(OLMRB_QUALITY, {
        const float quality = (float)def.u.fs_d.value;
        // Binary sub_180007AE0: double 1.0 / quality, cast to float; 0.2 when quality <= 0.
        p->quality_step_degrees = quality > 0.0f ? (float)(1.0 / (double)quality) : 0.2f;
    });
    OLMRB_READ(OLMRB_BRIGHTNESS, p->brightness = (float)def.u.fs_d.value);
    // Binary sub_180007AE0: float * float(0.01f) (dword_1800205F8 = 0x3C23D70A), not value / 100.
    OLMRB_READ(OLMRB_SIZE_VARIATION, {
        p->size_variation = (float)def.u.fs_d.value * 0.01f;
        p->size_variation_active = (double)p->size_variation > 0.0001;
    });
    OLMRB_READ(OLMRB_NOISE_VARIATION,
               p->noise_variation = (float)def.u.fs_d.value * 0.01f);
    OLMRB_READ(OLMRB_NOISE_TYPE, p->noise_type = def.u.pd.value);
    if (p->noise_type == OLMRB_NOISE_LAYER_MODE) {
        OLMRB_READ(OLMRB_NOISE_LAYER, {
            p->noise_layer = def.u.ld;
            p->has_noise_layer = true;
        });
    }
    OLMRB_READ(OLMRB_SEED, p->seed = def.u.sd.value);
    OLMRB_READ(OLMRB_NOISE_OFFSET,
               p->noise_offset_rad = (float)((double)def.u.ad.value * kFixedDegreesToRadians));
    OLMRB_READ(OLMRB_THICKNESS, p->thickness = (float)def.u.fs_d.value);

#undef OLMRB_READ
    return PF_Err_NONE;
}

PF_Err SetStreamHidden(AEGP_SuiteHandler &suites, AEGP_EffectRefH effectH,
                       PF_ParamIndex position, PF_Boolean hide) {
    AEGP_StreamRefH streamH = nullptr;
    PF_Err err = suites.StreamSuite7()->AEGP_GetNewEffectStreamByIndex(
        g_aegp_id, effectH, position, &streamH);
    if (!err && streamH) {
        AEGP_DynStreamFlags flags = 0;
        err = suites.DynamicStreamSuite3()->AEGP_GetDynamicStreamFlags(streamH, &flags);
        if (!err)
            err = suites.DynamicStreamSuite3()->AEGP_SetDynamicStreamFlag(
                streamH, AEGP_DynStreamFlag_HIDDEN, false, hide);
        suites.StreamSuite7()->AEGP_DisposeStream(streamH);
    }
    return err;
}

PF_Err SetParamDisabled(AEFX_SuiteScoper<PF_ParamUtilsSuite3> &suite,
                        PF_InData *in_data, PF_ParamIndex position,
                        const PF_ParamDef *current, PF_Boolean disabled) {
    if (!in_data || !current) return PF_Err_BAD_CALLBACK_PARAM;
    PF_ParamDef def = *current;
    if (disabled) def.ui_flags |= PF_PUI_DISABLED;
    else def.ui_flags &= ~PF_PUI_DISABLED;
    return suite->PF_UpdateParamUI(in_data->effect_ref, position, &def);
}

PF_Err UpdateParamsUI(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[]) {
    if (!in_data || !out_data || !params) return PF_Err_BAD_CALLBACK_PARAM;
    const PF_ParamIndex ui_positions[] = {
        OLMRB_OUTER_STRENGTH, OLMRB_OUTER_OFFSET_MODE, OLMRB_OUTER_OFFSET,
        OLMRB_INNER_STRENGTH, OLMRB_INNER_OFFSET_MODE, OLMRB_INNER_OFFSET
    };
    for (const PF_ParamIndex position : ui_positions)
        if (!params[position]) return PF_Err_BAD_CALLBACK_PARAM;
    if (!in_data->pica_basicP || !g_aegp_id) return PF_Err_NONE;
    PF_Err err = PF_Err_NONE;
    A_long blur_type = OLMRB_BLUR_ZOOM;
    A_long outer_mode = OLMRB_OFFSET_ADD;
    A_long inner_mode = OLMRB_OFFSET_ADD;
    A_long noise_type = OLMRB_NOISE_SMOOTH;
    err = checkout_value(in_data, OLMRB_BLUR_TYPE,
                         [&](const PF_ParamDef &def) { blur_type = def.u.pd.value; });
    if (err) return err;
    err = checkout_value(in_data, OLMRB_OUTER_OFFSET_MODE,
                         [&](const PF_ParamDef &def) { outer_mode = def.u.pd.value; });
    if (err) return err;
    err = checkout_value(in_data, OLMRB_INNER_OFFSET_MODE,
                         [&](const PF_ParamDef &def) { inner_mode = def.u.pd.value; });
    if (err) return err;
    err = checkout_value(in_data, OLMRB_NOISE_TYPE,
                         [&](const PF_ParamDef &def) { noise_type = def.u.pd.value; });
    if (err) return err;

    AEFX_SuiteScoper<PF_ParamUtilsSuite3> param_utils(
        in_data, kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3, out_data);
    const PF_Boolean rotation = blur_type == OLMRB_BLUR_ROTATION;
    err = SetParamDisabled(param_utils, in_data, OLMRB_OUTER_STRENGTH,
                           params[OLMRB_OUTER_STRENGTH],
                           rotation && outer_mode == OLMRB_OFFSET_OVERRIDE);
    if (err) return err;
    err = SetParamDisabled(param_utils, in_data, OLMRB_OUTER_OFFSET_MODE,
                           params[OLMRB_OUTER_OFFSET_MODE], !rotation);
    if (err) return err;
    err = SetParamDisabled(param_utils, in_data, OLMRB_OUTER_OFFSET,
                           params[OLMRB_OUTER_OFFSET], !rotation);
    if (err) return err;
    err = SetParamDisabled(param_utils, in_data, OLMRB_INNER_STRENGTH,
                           params[OLMRB_INNER_STRENGTH],
                           rotation && inner_mode == OLMRB_OFFSET_OVERRIDE);
    if (err) return err;
    err = SetParamDisabled(param_utils, in_data, OLMRB_INNER_OFFSET_MODE,
                           params[OLMRB_INNER_OFFSET_MODE], !rotation);
    if (err) return err;
    err = SetParamDisabled(param_utils, in_data, OLMRB_INNER_OFFSET,
                           params[OLMRB_INNER_OFFSET], !rotation);
    if (err) return err;

    AEGP_SuiteHandler suites(in_data->pica_basicP);
    AEGP_LayerH layerH = nullptr;
    err = suites.PFInterfaceSuite1()->AEGP_GetEffectLayer(in_data->effect_ref, &layerH);
    if (err || !layerH) return err;
    {
        A_Time layer_time;
        AEFX_CLR_STRUCT(layer_time);
        err = suites.LayerSuite5()->AEGP_GetLayerCurrentTime(
            layerH, AEGP_LTimeMode_LayerTime, &layer_time);
        if (err) return err;
    }
    AEGP_EffectRefH effectH = nullptr;
    err = suites.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(
        g_aegp_id, in_data->effect_ref, &effectH);
    if (err || !effectH) return err;

    const PF_Boolean is_layer = noise_type == OLMRB_NOISE_LAYER_MODE;
    err = SetStreamHidden(suites, effectH, OLMRB_NOISE_LAYER, !is_layer);
    if (!err) err = SetStreamHidden(suites, effectH, OLMRB_SEED, is_layer);
    if (!err) err = SetStreamHidden(suites, effectH, OLMRB_NOISE_OFFSET, is_layer);
    if (!err) err = SetStreamHidden(suites, effectH, OLMRB_THICKNESS, is_layer);
    suites.EffectSuite5()->AEGP_DisposeEffect(effectH);
    return err;
}

PF_Err PreRender(PF_InData *in_data, PF_PreRenderExtra *extra) {
    PF_CheckoutResult result;
    AEFX_CLR_STRUCT(result);
    PF_RenderRequest request = extra->input->output_request;
    PF_Err err = extra->cb->checkout_layer(in_data->effect_ref, OLMRB_INPUT, 0, &request,
                                            in_data->current_time, in_data->time_step,
                                            in_data->time_scale, &result);
    if (!err) {
        extra->output->flags |= PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS;
        UnionLRect(&result.result_rect, &extra->output->result_rect);
        UnionLRect(&result.max_result_rect, &extra->output->max_result_rect);
    }
    return err;
}

// MT19937 as used by the Windows binary (std::mt19937 initialization/twist).
struct Mt19937 {
    uint32_t state[624]{};
    int index = 625;

    void seed(uint32_t value) {
        state[0] = value;
        for (uint32_t i = 1; i < 624; ++i)
            state[i] = 1812433253u * (state[i - 1] ^ (state[i - 1] >> 30)) + i;
        index = 624;
    }

    uint32_t next() {
        if (index >= 624) {
            if (index == 625) seed(5489u);
            for (int i = 0; i < 624; ++i) {
                const uint32_t y = (state[i] & 0x80000000u) |
                                   (state[(i + 1) % 624] & 0x7fffffffu);
                state[i] = state[(i + 397) % 624] ^ (y >> 1) ^
                           ((y & 1u) ? 0x9908b0dfu : 0u);
            }
            index = 0;
        }
        uint32_t y = state[index++];
        y ^= y >> 11;
        y ^= (y << 7) & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        y ^= y >> 18;
        return y;
    }

    float unit() { return (float)next() / 4294967296.0f; }
};

struct NoiseField {
    int width = 0;
    int height = 0;
    float cell = 1.0f;
    float inv_cell = 1.0f;
    std::vector<float> table;
    std::vector<float> values;
};

void BuildNoiseField(NoiseField &field, int width, int height, float cell,
                     float offset, int seed) {
    field.cell = cell;
    // Binary sub_1800086E0/sub_1800089E0: reciprocal is double 1.0/cell cast to float, then multiplied.
    const float inv_cell = (float)(1.0 / (double)cell);
    field.inv_cell = inv_cell;
    field.width = (int)((float)width * inv_cell + 3.0f);
    field.height = (int)((float)height * inv_cell + 3.0f);
    Mt19937 rng;
    rng.seed((uint32_t)seed);
    field.table.resize(101);
    // The binary consumes all table draws before drawing any cell values.
    for (int i = 0; i < 101; ++i)
        field.table[(size_t)i] = 2.0f * rng.unit() - 1.0f;

    field.values.resize((size_t)field.width * (size_t)field.height);
    for (int y = 0; y < field.height; ++y) {
        for (int x = 0; x < field.width; ++x) {
            const float white = rng.unit();
            float t = 100.0f * rng.unit() + offset;
            while (t >= 100.0f) t -= 100.0f;
            // ponytail: wrap the original negative table underflow; exact OOB bytes are unknown.
            while (t < 0.0f) t += 100.0f;
            const int i = (int)t;
            const float f = t - (float)i;
            const float smooth = f * f * (3.0f - 2.0f * f);
            const float left = (1.0f - smooth) * field.table[(size_t)i];
            const float right = smooth * field.table[(size_t)i + 1];
            const float noise = white + (left * 0.5f + right * 0.5f);
            field.values[(size_t)y * (size_t)field.width + (size_t)x] =
                std::max(0.0f, std::min(1.0f, noise));
        }
    }
}

float SampleNoiseField(const NoiseField &field, float x, float y, bool smooth, bool repeat) {
    if (!field.width || !field.height) return 0.0f;
    float gx = x * field.inv_cell;
    float gy = y * field.inv_cell;
    if (repeat) {
        gx = std::max(0.0f, std::min((float)(field.width - 1), gx));
        gy = std::max(0.0f, std::min((float)(field.height - 1), gy));
    } else if (gx < 0.0f || gy < 0.0f || gx >= (float)field.width - 1.0f ||
               gy >= (float)field.height - 1.0f) {
        return 0.0f;
    }
    int ix = (int)gx;
    int iy = (int)gy;
    ix = std::max(0, std::min(field.width - 2, ix));
    iy = std::max(0, std::min(field.height - 2, iy));
    if (!smooth)
        return field.values[(size_t)iy * (size_t)field.width + (size_t)ix];
    const float fx = gx - (float)ix;
    const float fy = gy - (float)iy;
    const float wx = fx * fx * (3.0f - 2.0f * fx);
    const float wy = fy * fy * (3.0f - 2.0f * fy);
    const float *row = field.values.data() + (size_t)iy * (size_t)field.width + (size_t)ix;
    // Binary tap order: (x1y0 + x0y0) + (x1y1 + x0y1).
    return (((1.0f - wy) * wx) * row[1] + ((1.0f - wx) * (1.0f - wy)) * row[0]) +
           ((wy * wx) * row[field.width + 1] + ((1.0f - wx) * wy) * row[field.width]);
}

// RadialBlur's scalar LUT order is intentionally distinct from DirectionalBlur.
// The SIMD reciprocal/Newton branch is omitted; scalar values differ by at most ~1 ulp.
void BuildLut(std::vector<float> &lut, int n) {
    if (n <= 0) {
        lut.clear();
        return;
    }
    lut.resize((size_t)n);
    const float n_f = (float)n;
    const float t = n_f * n_f * 0.11111112f;
    const float den0 = t + t;
    const float denom = (float)((double)den0 + 0.00001);
    const float inv = (float)(1.0 / (double)denom);
    for (int i = 0; i < n; ++i)
        lut[(size_t)i] = expf(-(float)(i * i) * inv);
}

struct Run {
    int y;
    int x0;
    int x1;
    int length;
};

void BuildComponents(int width, int height, const std::vector<unsigned char> &mask,
                     std::vector<float> &area_map, float *max_area) {
    std::vector<int> row_start((size_t)height + 1, 0);
    std::vector<Run> runs;
    for (int y = 0; y < height; ++y) {
        row_start[(size_t)y] = (int)runs.size();
        int x = 0;
        while (x < width) {
            if (!mask[(size_t)y * (size_t)width + (size_t)x]) {
                ++x;
                continue;
            }
            const int x0 = x;
            while (x + 1 < width && mask[(size_t)y * (size_t)width + (size_t)(x + 1)]) ++x;
            runs.push_back(Run{y, x0, x, x - x0 + 1});
            ++x;
        }
    }
    row_start[(size_t)height] = (int)runs.size();

    const int run_count = (int)runs.size();
    if (!run_count) {
        *max_area = 0.0f;
        return;
    }
    std::vector<int> link_count((size_t)run_count, 0);
    const auto overlaps = [](const Run &a, const Run &b) {
        return a.x0 <= b.x1 && b.x0 <= a.x1;
    };
    for (int y = 1; y < height; ++y) {
        for (int i = row_start[(size_t)y]; i < row_start[(size_t)y + 1]; ++i) {
            for (int j = row_start[(size_t)y - 1]; j < row_start[(size_t)y]; ++j) {
                if (overlaps(runs[(size_t)i], runs[(size_t)j])) {
                    ++link_count[(size_t)i];
                    ++link_count[(size_t)j];
                }
            }
        }
    }
    std::vector<int> adj_begin((size_t)run_count + 1, 0);
    for (int i = 0; i < run_count; ++i)
        adj_begin[(size_t)i + 1] = adj_begin[(size_t)i] + link_count[(size_t)i];
    std::vector<int> adj((size_t)adj_begin[(size_t)run_count]);
    std::vector<int> fill_pos(adj_begin.begin(), adj_begin.end() - 1);
    for (int y = 1; y < height; ++y) {
        for (int i = row_start[(size_t)y]; i < row_start[(size_t)y + 1]; ++i) {
            for (int j = row_start[(size_t)y - 1]; j < row_start[(size_t)y]; ++j) {
                if (overlaps(runs[(size_t)i], runs[(size_t)j])) {
                    adj[(size_t)fill_pos[(size_t)i]++] = j;
                    adj[(size_t)fill_pos[(size_t)j]++] = i;
                }
            }
        }
    }

    std::vector<unsigned char> visited((size_t)run_count, 0);
    std::vector<int> stack, members;
    *max_area = 0.0f;
    for (int root = 0; root < run_count; ++root) {
        if (visited[(size_t)root]) continue;
        stack.clear();
        members.clear();
        stack.push_back(root);
        visited[(size_t)root] = 1;
        int area = 0;
        while (!stack.empty()) {
            const int id = stack.back();
            stack.pop_back();
            members.push_back(id);
            area += runs[(size_t)id].length;
            for (int k = adj_begin[(size_t)id]; k < adj_begin[(size_t)id + 1]; ++k) {
                const int child = adj[(size_t)k];
                if (!visited[(size_t)child]) {
                    visited[(size_t)child] = 1;
                    stack.push_back(child);
                }
            }
        }
        const float component_area = (float)area;
        for (int id : members) {
            const Run &run = runs[(size_t)id];
            for (int x = run.x0; x <= run.x1; ++x)
                area_map[(size_t)run.y * (size_t)width + (size_t)x] = component_area;
        }
        *max_area = std::max(*max_area, component_area);
    }
}

struct Image {
    int width = 0;
    int height = 0;
    std::vector<float> rgba;

    size_t pixel(int x, int y) const { return (size_t)y * (size_t)width + (size_t)x; }
};

template <typename Pixel, typename Channel>
void DecodeWorld(const PF_EffectWorld *world, float divisor, Image &image) {
    image.width = world->width;
    image.height = world->height;
    image.rgba.assign((size_t)image.width * (size_t)image.height * 4u, 0.0f);
    // Binary decode (sub_180016760): channel * float(1/255), i.e. a reciprocal multiply, not a divide.
    const float scale = (float)(1.0 / (double)divisor);
    for (int y = 0; y < image.height; ++y) {
        const char *row = (const char *)world->data + (size_t)y * (size_t)world->rowbytes;
        const Pixel *pixels = (const Pixel *)row;
        for (int x = 0; x < image.width; ++x) {
            const Pixel &src = pixels[x];
            float *dst = &image.rgba[image.pixel(x, y) * 4u];
            dst[0] = (float)(Channel)src.red * scale;
            dst[1] = (float)(Channel)src.green * scale;
            dst[2] = (float)(Channel)src.blue * scale;
            dst[3] = (float)(Channel)src.alpha * scale;
        }
    }
}

void DecodeWorld32(const PF_EffectWorld *world, Image &image) {
    image.width = world->width;
    image.height = world->height;
    image.rgba.assign((size_t)image.width * (size_t)image.height * 4u, 0.0f);
    for (int y = 0; y < image.height; ++y) {
        const char *row = (const char *)world->data + (size_t)y * (size_t)world->rowbytes;
        const PF_PixelFloat *pixels = (const PF_PixelFloat *)row;
        for (int x = 0; x < image.width; ++x) {
            const PF_PixelFloat &src = pixels[x];
            float *dst = &image.rgba[image.pixel(x, y) * 4u];
            dst[0] = src.red;
            dst[1] = src.green;
            dst[2] = src.blue;
            dst[3] = src.alpha;
        }
    }
}

float ClampEdgeCoordinate(float value, int size) {
    return std::max(0.0f, std::min((float)(size - 1), value));
}

// Windows binary samplers (sub_180009340 non-repeat, sub_1800095F0 repeat). Repeat clamps every tap
// to the image edge and never rejects a position; the returned flag is "in range and alpha sum != 0"
// and gates the polar scatter. Non-repeat drops taps outside the image and normalises alpha by coverage.
bool SampleRgba(const Image &image, float x, float y, bool repeat, float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (image.width <= 0 || image.height <= 0) return false;
    const int x0 = (int)x;
    const int y0 = (int)y;
    const bool in_range = x0 > -2 && y0 > -2 && x0 < image.width && y0 < image.height;
    if (!repeat && !in_range) return false;

    const float fx = x - (float)x0;
    const float fy = y - (float)y0;
    float coverage = 0.0f;
    float alpha = 0.0f;
    float rgb[3] = {0.0f, 0.0f, 0.0f};
    for (int dy = 0; dy <= 1; ++dy) {
        const int sy = repeat ? std::max(0, std::min(image.height - 1, y0 + dy)) : y0 + dy;
        if (sy < 0 || sy >= image.height) continue;
        const float wy = dy ? fy : 1.0f - fy;
        for (int dx = 0; dx <= 1; ++dx) {
            const int sx = repeat ? std::max(0, std::min(image.width - 1, x0 + dx)) : x0 + dx;
            if (sx < 0 || sx >= image.width) continue;
            const float wx = dx ? fx : 1.0f - fx;
            const float weight = wx * wy;
            const float *src = &image.rgba[image.pixel(sx, sy) * 4u];
            coverage += weight;
            const float alpha_weight = weight * src[3];
            alpha += alpha_weight;
            rgb[0] += alpha_weight * src[0];
            rgb[1] += alpha_weight * src[1];
            rgb[2] += alpha_weight * src[2];
        }
    }
    if (repeat) {
        out[3] = alpha;  // coverage is 1 for clamped taps
    } else if (coverage != 0.0f) {
        out[3] = alpha / coverage;
    }
    if (alpha != 0.0f) {
        const float inv = (float)(1.0 / (double)alpha);
        out[0] = rgb[0] * inv;
        out[1] = rgb[1] * inv;
        out[2] = rgb[2] * inv;
    }
    return in_range && alpha != 0.0f;
}

// Windows binary repeat factor sampler (sub_180009A20). Corners are clamped to the image, the fractions
// come from the unclamped truncation, and the two right-hand taps read the flat index x1 + 1 (the binary
// adds one float to the x1 address). Summed in the binary's order with no normalisation.
float SampleScalarRepeat(const std::vector<float> &values, int width, int height, float x, float y) {
    const int xi = (int)x;
    const int yi = (int)y;
    const int x0 = std::max(0, std::min(xi, width - 1));
    const int x1 = std::max(0, std::min(xi + 1, width - 1));
    const int y0 = std::max(0, std::min(yi, height - 1));
    const int y1 = std::max(0, std::min(yi + 1, height - 1));
    const float fx = x - (float)xi;
    const float fy = y - (float)yi;
    const size_t n = values.size();
    auto at = [&](long long idx) -> float {
        return idx >= 0 && (size_t)idx < n ? values[(size_t)idx] : 0.0f;
    };
    const float t1 = ((1.0f - fx) * (1.0f - fy)) * at((long long)y0 * width + x0);
    const float t2 = ((1.0f - fy) * fx) * at((long long)y0 * width + x1 + 1);
    const float t3 = ((1.0f - fx) * fy) * at((long long)y1 * width + x0);
    const float t4 = (fy * fx) * at((long long)y1 * width + x1 + 1);
    float sum = t1;
    sum = t2 + sum;
    sum = t3 + sum;
    sum = t4 + sum;
    return sum;
}

float SampleScalar(const std::vector<float> &values, int width, int height,
                   float x, float y, bool repeat) {
    if (repeat) return SampleScalarRepeat(values, width, height, x, y);
    if (width <= 0 || height <= 0) return 0.0f;
    const int x0 = (int)x;
    const int y0 = (int)y;
    if (!repeat && (x0 <= -2 || y0 <= -2 || x0 >= width || y0 >= height)) {
        return 0.0f;
    }
    const float fx = x - (float)x0;
    const float fy = y - (float)y0;
    float sum = 0.0f;
    float coverage = 0.0f;
    for (int dy = 0; dy <= 1; ++dy) {
        const int sy = repeat ? std::max(0, std::min(height - 1, y0 + dy)) : y0 + dy;
        if (sy < 0 || sy >= height) continue;
        const float wy = dy ? fy : 1.0f - fy;
        for (int dx = 0; dx <= 1; ++dx) {
            const int sx = repeat ? std::max(0, std::min(width - 1, x0 + dx)) : x0 + dx;
            if (sx < 0 || sx >= width) continue;
            const float wx = dx ? fx : 1.0f - fx;
            const float weight = wx * wy;
            sum += weight * values[(size_t)sy * (size_t)width + (size_t)sx];
            coverage += weight;
        }
    }
    return coverage != 0.0f ? sum / coverage : 0.0f;
}

float ReadNoisePixel(const PF_EffectWorld &world, int x, int y, int bitdepth) {
    if (!world.data || x < 0 || y < 0 || x >= world.width || y >= world.height) return 0.0f;
    const char *row = (const char *)world.data + (size_t)y * (size_t)world.rowbytes;
    float r, g, b, a;
    if (bitdepth == 8) {
        const PF_Pixel *p = (const PF_Pixel *)row + x;
        r = (float)p->red / 255.0f; g = (float)p->green / 255.0f;
        b = (float)p->blue / 255.0f; a = (float)p->alpha / 255.0f;
    } else if (bitdepth == 16) {
        const PF_Pixel16 *p = (const PF_Pixel16 *)row + x;
        r = (float)p->red / 32768.0f; g = (float)p->green / 32768.0f;
        b = (float)p->blue / 32768.0f; a = (float)p->alpha / 32768.0f;
    } else {
        const PF_PixelFloat *p = (const PF_PixelFloat *)row + x;
        r = p->red; g = p->green; b = p->blue; a = p->alpha;
    }
    const float premul_r = r * a;
    const float premul_g = g * a;
    const float premul_b = b * a;
    return (float)((double)premul_r * 0.299 + (double)premul_g * 0.587 +
                   (double)premul_b * 0.114);
}

struct PolarBounds {
    int low_radius = 0;
    int rows = 0;
    int angle_count = 0;
    float angle_step = 0.0f;
};

PolarBounds GetPolarBounds(const OLMRBParams &p, const Image &image, float cx, float cy) {
    const float dx_min = cx < 0.0f ? -cx : (cx >= image.width ? cx - (float)image.width : 0.0f);
    const float dy_min = cy < 0.0f ? -cy : (cy >= image.height ? cy - (float)image.height : 0.0f);
    const float dx_max = std::max(fabsf(cx), fabsf((float)image.width - cx));
    const float dy_max = std::max(fabsf(cy), fabsf((float)image.height - cy));
    const int min_dist = (int)sqrtf(dx_min * dx_min + dy_min * dy_min);
    const int max_dist = (int)sqrtf(dx_max * dx_max + dy_max * dy_max);
    int low = (int)((float)min_dist / p.ratio) - 2;
    if (low < 0) low = 0;
    const int high = max_dist + 2;
    PolarBounds bounds;
    bounds.low_radius = low;
    bounds.rows = high - low + 1;
    bounds.angle_step = p.quality_step_degrees;
    bounds.angle_count = bounds.angle_step > 0.0f ? (int)(360.0f / bounds.angle_step) : 0;
    bounds.angle_step *= kDegreesToRadians;
    return bounds;
}

size_t ZoomIndex(const PolarBounds &b, int angle, int radius) {
    return (size_t)angle * (size_t)b.rows + (size_t)radius;
}

size_t RotationIndex(const PolarBounds &b, int radius, int angle) {
    return (size_t)radius * (size_t)b.angle_count + (size_t)angle;
}

void FillPolar(const Image &image, const std::vector<float> &factor_size,
               const std::vector<float> &factor_b, const OLMRBParams &p,
               const PolarBounds &bounds, float cx, float cy, bool zoom,
               std::vector<float> &polar, std::vector<float> &polar_size,
               std::vector<float> &polar_factor, std::vector<unsigned char> &polar_mask) {
    const size_t count = (size_t)bounds.rows * (size_t)bounds.angle_count;
    polar.assign(count * 4u, 0.0f);
    polar_size.assign(count, 0.0f);
    polar_factor.assign(count, 0.0f);
    polar_mask.assign(count, 0);
    const float ca = cosf(p.angle_rad);
    const float sa = sinf(p.angle_rad);
    for (int outer = 0; outer < bounds.angle_count; ++outer) {
        const float theta = (float)outer * bounds.angle_step;
        const float ct = cosf(theta);
        const float st = sinf(theta);
        for (int inner = 0; inner < bounds.rows; ++inner) {
            const int radius_index = inner;
            const float radius = (float)(bounds.low_radius + radius_index);
            const float radial_x = radius * ct;
            // Binary association (sub_180005070): r * (sin * ratio), then (cos*rx - sin*ry) + cx.
            const float radial_y = radius * (st * p.ratio);
            const float x = (ca * radial_x - sa * radial_y) + cx;
            const float y = (sa * radial_x + ca * radial_y) + cy;
            const size_t i = zoom ? ZoomIndex(bounds, outer, inner)
                                  : RotationIndex(bounds, inner, outer);
            polar_mask[i] = SampleRgba(image, x, y, p.repeat_border, &polar[i * 4u]) ? 1 : 0;
            // Binary (sub_180005070): the size factor is sampled only when size variation is active;
            // otherwise the fade reach is exactly 1.0 (a bilinear sum of 1.0 is not exactly 1.0 in float).
            polar_size[i] = p.size_variation_active
                ? SampleScalar(factor_size, image.width, image.height, x, y, p.repeat_border)
                : 1.0f;
            polar_factor[i] = SampleScalar(factor_b, image.width, image.height,
                                           x, y, p.repeat_border);
        }
    }
}

void FadePolar(const PolarBounds &bounds, bool zoom, int low_fade, int high_fade,
               const std::vector<float> &low_lut, const std::vector<float> &high_lut,
               const std::vector<float> &source, const std::vector<float> &factor,
               std::vector<float> &base, std::vector<float> &weight_sum,
               std::vector<float> &max_alpha) {
    const size_t count = (size_t)bounds.rows * (size_t)bounds.angle_count;
    base.assign(count * 4u, 0.0f);
    weight_sum.assign(count, 0.0f);
    max_alpha.assign(count, 0.0f);
    for (int angle = 0; angle < bounds.angle_count; ++angle) {
        for (int radius = 0; radius < bounds.rows; ++radius) {
            const size_t i = zoom ? ZoomIndex(bounds, angle, radius)
                                  : RotationIndex(bounds, radius, angle);
            const float f = factor[i];
            const float inv_f = (float)(1.0 / (double)f);  // binary: 1.0 / reach in double
            const float *src = &source[i * 4u];
            if (src[3] == 0.0f || f == 0.0f) continue;
            float sum = 1.0f;
            float alpha_sum = src[3];
            const int axis = zoom ? radius : angle;
            const int axis_size = zoom ? bounds.rows : bounds.angle_count;
            const int low_reach_raw = (int)((float)low_fade * f);
            const int high_reach_raw = (int)((float)high_fade * f);
            const int low_reach = zoom ? std::min(low_reach_raw, axis) :
                                         std::min(low_reach_raw, axis_size);
            const int high_reach = zoom ? std::min(high_reach_raw, axis_size - axis) :
                                          std::min(high_reach_raw, axis_size);
            for (int k = 1; k < low_reach; ++k) {
                const int lut_i = (int)((float)k * inv_f);
                if (lut_i < 0 || (size_t)lut_i >= low_lut.size()) break;
                int neighbour_axis = axis - k;
                if (!zoom) {
                    neighbour_axis %= axis_size;
                    if (neighbour_axis < 0) neighbour_axis += axis_size;
                }
                const size_t neighbour = zoom ? ZoomIndex(bounds, angle, neighbour_axis)
                                              : RotationIndex(bounds, radius, neighbour_axis);
                const float tap = low_lut[(size_t)lut_i];
                sum += tap;
                alpha_sum += tap * source[neighbour * 4u + 3u];
            }
            for (int k = 1; k < high_reach; ++k) {
                const int lut_i = (int)((float)k * inv_f);
                if (lut_i < 0 || (size_t)lut_i >= high_lut.size()) break;
                int neighbour_axis = axis + k;
                if (!zoom) neighbour_axis %= axis_size;
                const size_t neighbour = zoom ? ZoomIndex(bounds, angle, neighbour_axis)
                                              : RotationIndex(bounds, radius, neighbour_axis);
                const float tap = high_lut[(size_t)lut_i];
                sum += tap;
                alpha_sum += tap * source[neighbour * 4u + 3u];
            }
            const float avg_alpha = alpha_sum / sum;
            float *dst = &base[i * 4u];
            dst[0] = avg_alpha * src[0];
            dst[1] = avg_alpha * src[1];
            dst[2] = avg_alpha * src[2];
            dst[3] = avg_alpha;
            weight_sum[i] = avg_alpha;
            max_alpha[i] = avg_alpha;
        }
    }
}

void ScatterZoomDirection(const PolarBounds &bounds, int angle, int radius, int strength,
                          float factor, bool toward_lower, const std::vector<float> &lut,
                          const float source[4], float center_alpha, std::vector<float> &dst,
                          std::vector<float> &weight_sum, std::vector<float> &max_alpha) {
    if (strength <= 0 || factor <= 0.0f || center_alpha == 0.0f) return;
    int reach = (int)((float)strength * factor);
    reach = std::min(reach, toward_lower ? radius : bounds.rows - radius);
    const float inv_factor = (float)(1.0 / (double)factor);  // binary: 1.0 / reach in double
    for (int k = 1; k < reach; ++k) {
        const int lut_i = (int)((float)k * inv_factor);
        if (lut_i < 0 || (size_t)lut_i >= lut.size()) break;
        const int target_radius = toward_lower ? radius - k : radius + k;
        const size_t target = ZoomIndex(bounds, angle, target_radius);
        const float w = center_alpha * lut[(size_t)lut_i];
        float *pixel = &dst[target * 4u];
        pixel[0] += w * source[0];
        pixel[1] += w * source[1];
        pixel[2] += w * source[2];
        weight_sum[target] += w;
        max_alpha[target] = std::max(max_alpha[target], w);
    }
}

void ScatterZoom(const PolarBounds &bounds, int outer_strength, int inner_strength,
                 const std::vector<float> &outer_lut,
                 const std::vector<float> &inner_lut, const std::vector<float> &source,
                 const std::vector<unsigned char> &mask, const std::vector<float> &factor,
                 std::vector<float> &base, std::vector<float> &weight_sum,
                 std::vector<float> &max_alpha) {
    for (int angle = 0; angle < bounds.angle_count; ++angle) {
        for (int radius = 0; radius < bounds.rows; ++radius) {
            const size_t i = ZoomIndex(bounds, angle, radius);
            if (!mask[i]) continue;  // sampler validity gates the scatter (binary sub_180009D50)
            const float *src = &source[i * 4u];
            const float center_alpha = base[i * 4u + 3u];
            ScatterZoomDirection(bounds, angle, radius, inner_strength, factor[i], true,
                                 inner_lut, src, center_alpha, base, weight_sum, max_alpha);
            ScatterZoomDirection(bounds, angle, radius, outer_strength, factor[i], false,
                                 outer_lut, src, center_alpha, base, weight_sum, max_alpha);
        }
    }
}

void NormalizePolar(size_t count, std::vector<float> &rgba,
                    const std::vector<float> &weight_sum,
                    const std::vector<float> &max_alpha) {
    for (size_t i = 0; i < count; ++i) {
        // Binary tail of sub_180005070: the colour is zeroed when the max lane is 0, otherwise
        // divided by the accumulated weight (which may be negative for out-of-range repeat taps).
        const float weight = weight_sum[i];
        float *pixel = &rgba[i * 4u];
        if (max_alpha[i] != 0.0f) {
            pixel[0] /= weight;
            pixel[1] /= weight;
            pixel[2] /= weight;
        } else {
            pixel[0] = pixel[1] = pixel[2] = 0.0f;
        }
        pixel[3] = max_alpha[i];
    }
}

void SamplePolarBilinear(const PolarBounds &bounds, bool zoom, float angle, float radius,
                         const std::vector<float> &polar, float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (bounds.rows <= 0 || bounds.angle_count <= 0 || radius < 0.0f ||
        radius > (float)(bounds.rows - 1)) return;
    angle = fmodf(angle, (float)bounds.angle_count);
    if (angle < 0.0f) angle += (float)bounds.angle_count;
    const int a0 = (int)angle;
    const int a1 = (a0 + 1) % bounds.angle_count;
    const int r0 = (int)radius;
    const int r1 = std::min(r0 + 1, bounds.rows - 1);
    const float fa = angle - (float)a0;
    const float fr = radius - (float)r0;
    float alpha_sum = 0.0f;
    float rgb[3] = {0.0f, 0.0f, 0.0f};
    for (int da = 0; da <= 1; ++da) {
        const int aa = da ? a1 : a0;
        const float wa = da ? fa : 1.0f - fa;
        for (int dr = 0; dr <= 1; ++dr) {
            const int rr = dr ? r1 : r0;
            const float wr = dr ? fr : 1.0f - fr;
            const size_t i = zoom ? ZoomIndex(bounds, aa, rr) : RotationIndex(bounds, rr, aa);
            const float w = wr * wa * polar[i * 4u + 3u];
            alpha_sum += w;
            rgb[0] += w * polar[i * 4u];
            rgb[1] += w * polar[i * 4u + 1u];
            rgb[2] += w * polar[i * 4u + 2u];
        }
    }
    if (alpha_sum != 0.0f) {
        // Windows binary (sub_180009100): colour * (1 / alpha_sum), reciprocal computed in double.
        const float inv = (float)(1.0 / (double)alpha_sum);
        out[0] = rgb[0] * inv;
        out[1] = rgb[1] * inv;
        out[2] = rgb[2] * inv;
    }
    out[3] = alpha_sum;
}

void WriteWorld(PF_EffectWorld *output, int bitdepth, const std::vector<float> &rgba,
                int width, int height, float brightness) {
    for (int y = 0; y < output->height; ++y) {
        char *row = (char *)output->data + (size_t)y * (size_t)output->rowbytes;
        for (int x = 0; x < output->width; ++x) {
            // Binary writer (sub_180006970 tail): the core buffer is indexed with the OUTPUT width as its
            // row stride, while the core computed it on the input grid.
            const float *src = nullptr;
            const size_t index = (size_t)y * (size_t)width + (size_t)x;
            if (index < rgba.size() / 4u)
                src = &rgba[index * 4u];
            const float r = src ? src[0] : 0.0f;
            const float g = src ? src[1] : 0.0f;
            const float b = src ? src[2] : 0.0f;
            const float a = src ? src[3] : 0.0f;
            if (bitdepth == 8) {
                PF_Pixel *pixel = (PF_Pixel *)row + x;
                pixel->red = (A_u_char)(int)(fminf(brightness * r, 1.0f) * 255.0f);
                pixel->green = (A_u_char)(int)(fminf(brightness * g, 1.0f) * 255.0f);
                pixel->blue = (A_u_char)(int)(fminf(brightness * b, 1.0f) * 255.0f);
                pixel->alpha = (A_u_char)(int)(a * 255.0f);
            } else if (bitdepth == 16) {
                PF_Pixel16 *pixel = (PF_Pixel16 *)row + x;
                pixel->red = (A_u_short)(int)(fminf(brightness * r, 1.0f) * 32768.0f);
                pixel->green = (A_u_short)(int)(fminf(brightness * g, 1.0f) * 32768.0f);
                pixel->blue = (A_u_short)(int)(fminf(brightness * b, 1.0f) * 32768.0f);
                pixel->alpha = (A_u_short)(int)(a * 32768.0f);
            } else {
                PF_PixelFloat *pixel = (PF_PixelFloat *)row + x;
                pixel->red = fminf(brightness * r, 1.0f);
                pixel->green = fminf(brightness * g, 1.0f);
                pixel->blue = fminf(brightness * b, 1.0f);
                pixel->alpha = a;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Rotation pipeline, transcribed from the binary rotation core sub_1800046B0 and its helpers:
// colour samplers sub_180001270 (non-repeat) / sub_180001520 (repeat), factor samplers
// sub_180001800 / sub_180001950, bounds sub_180001BB0, pixel map sub_180001B10, writer
// sub_180001000, fade sub_180002780, scatter sub_1800024C0 + sub_180001C90.
// Cell (r, t) = r * angles + t: r is the radius ring (0 at the lowest radius), t the angle index.
// ---------------------------------------------------------------------------------------------
struct RotInputs {
    int w = 0, h = 0;
    const float *rgba = nullptr;         // input RGBA, w*h*4 (binary work+152)
    const float *factor_size = nullptr;  // size factor, w*h (work+136), read when size_active
    const float *factor_comb = nullptr;  // combined factor, w*h (work+144)
    bool size_active = false;            // work+68
    double cx = 0.0, cy = 0.0;           // work+40 / work+48: origin-adjusted, scaled centre
    float ratio = 1.0f;                  // work+120
    float angle_rad = 0.0f;              // work+124
    float quality_step = 1.0f;           // work+128 (degrees per step, already 1/quality)
    int outer_mode = 0, outer_offset = 0;       // work+84 / work+88
    int inner_mode = 0, inner_offset = 0;       // work+92 / work+96
    int outer_strength = 0, inner_strength = 0; // work+100 / work+104
    int outer_fade = 0, inner_fade = 0;         // work+108 / work+112
    bool repeat = false;                        // work+116
};

// sub_180001270: colour sampler, non-repeat. Out-of-range taps are dropped and the alpha is normalised by
// coverage. Returns validity (in range and accumulated alpha != 0).
static int RotSampleColourNR(const float *src, int w, int h, float x, float y, float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    const int x0 = (int)x, y0 = (int)y;
    if (x0 <= -2 || x0 >= w || y0 <= -2 || y0 >= h) return 0;
    const float fx = x - (float)x0;
    const float fy = y - (float)y0;
    float cover = 0.0f;
    // One tap: alpha-weighted accumulation in binary order (alpha, B, G, R).
    auto tap = [&](int xx, int yy, float wgt) {
        const float *p = src + 4 * ((size_t)yy * (size_t)w + (size_t)xx);
        const float a = wgt * p[3];
        out[3] = a + out[3];
        out[2] = (a * p[2]) + out[2];
        out[1] = (a * p[1]) + out[1];
        out[0] = (a * p[0]) + out[0];
    };
    if (y0 >= 0) {
        const float wy0 = 1.0f - fy;
        if (x0 >= 0) {
            cover = (1.0f - fx) * wy0;
            tap(x0, y0, cover);
        }
        if (x0 + 1 < w) {
            cover = cover + (wy0 * fx);
            tap(x0 + 1, y0, wy0 * fx);
        }
    }
    if (y0 + 1 < h) {
        if (x0 >= 0) {
            const float wy1 = (1.0f - fx) * fy;
            cover = cover + wy1;
            tap(x0, y0 + 1, wy1);
        }
        if (x0 + 1 < w) {
            cover = cover + (fy * fx);
            tap(x0 + 1, y0 + 1, fy * fx);
        }
    }
    const float alpha = out[3];
    if (alpha == 0.0f) return 0;
    const float inv = (float)(1.0 / (double)alpha);
    out[0] = out[0] * inv;
    out[1] = out[1] * inv;
    out[2] = inv * out[2];
    out[3] = alpha / cover;
    return 1;
}

// sub_180001520: colour sampler, repeat (clamped taps, full coverage). Validity: x0 in (-2, w), y0 in (-2, h).
static int RotSampleColourRep(const float *src, int w, int h, float x, float y, float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    const int x0 = (int)x, y0 = (int)y;
    bool xok = false;
    if (x0 < w) xok = x0 > -2;
    const bool valid_xy = (y0 > -2) ? xok : false;
    int xc0 = x0 > w - 1 ? w - 1 : x0;
    int xc1 = x0 + 1 > w - 1 ? w - 1 : x0 + 1;
    int yc0 = y0 > h - 1 ? h - 1 : y0;
    int yc1 = y0 + 1 > h - 1 ? h - 1 : y0 + 1;
    yc0 = yc0 > 0 ? yc0 : 0;
    yc1 = yc1 > 0 ? yc1 : 0;
    xc0 = xc0 > 0 ? xc0 : 0;
    xc1 = xc1 > 0 ? xc1 : 0;
    const float fx = x - (float)x0;
    const float fy = y - (float)y0;
    const float omx = 1.0f - fx;
    const float omy = 1.0f - fy;
    auto tap = [&](int xx, int yy, float wgt) {
        const float *p = src + 4 * ((size_t)yy * (size_t)w + (size_t)xx);
        const float a = wgt * p[3];
        out[3] = a + out[3];
        out[2] = (a * p[2]) + out[2];
        out[1] = (a * p[1]) + out[1];
        out[0] = (a * p[0]) + out[0];
    };
    tap(xc0, yc0, omx * omy);
    tap(xc1, yc0, omy * fx);
    tap(xc0, yc1, omx * fy);
    tap(xc1, yc1, fy * fx);
    const float alpha = out[3];
    if (alpha == 0.0f) return 0;
    const int valid = (y0 < h) ? (valid_xy ? 1 : 0) : 0;
    const float inv = (float)(1.0 / (double)alpha);
    out[0] = out[0] * inv;
    out[1] = out[1] * inv;
    out[2] = inv * out[2];
    return valid;
}

// sub_180001800: factor sampler, non-repeat. Weighted mean over in-range taps; 0 when out of range.
static float RotSampleFactorNR(const float *src, int w, int h, float x, float y) {
    const int x0 = (int)x, y0 = (int)y;
    if (!(x0 > -2 && x0 < w && y0 > -2 && y0 < h)) return 0.0f;
    const float fx = x - (float)x0;
    const float fy = y - (float)y0;
    auto at = [&](int dx, int dy) -> float {
        return src[(size_t)(x0 + dx) + (size_t)w * (size_t)(y0 + dy)];
    };
    float wsum = 0.0f;
    float acc = 0.0f;
    float out = 0.0f;
    if (y0 >= 0) {
        const float wy0 = 1.0f - fy;
        if (x0 >= 0) {
            wsum = (1.0f - fx) * wy0;
            acc = wsum * at(0, 0);
            out = acc;
        }
        if (x0 + 1 < w) {
            const float t = wy0 * fx;
            wsum = wsum + t;
            acc = (t * at(1, 0)) + acc;
            out = acc;
        }
    }
    if (y0 + 1 < h) {
        if (x0 >= 0) {
            const float t = (1.0f - fx) * fy;
            wsum = wsum + t;
            acc = (t * at(0, 1)) + acc;
            out = acc;
        }
        if (x0 + 1 < w) {
            wsum = wsum + (fy * fx);
            acc = ((fy * fx) * at(1, 1)) + acc;
            out = acc;
        }
    }
    if (wsum != 0.0f) out = acc / wsum;
    return out;
}

// sub_180001950: factor sampler, repeat. Clamped taps; the x1 taps read flat index +1 (binary quirk).
static float RotSampleFactorRep(const float *src, int w, int h, float x, float y) {
    const int x0 = (int)x, y0 = (int)y;
    int xc0 = x0 > w - 1 ? w - 1 : x0;
    int xc1 = x0 + 1 > w - 1 ? w - 1 : x0 + 1;
    int yc0 = y0 > h - 1 ? h - 1 : y0;
    int yc1 = y0 + 1 > h - 1 ? h - 1 : y0 + 1;
    yc0 = yc0 > 0 ? yc0 : 0;
    yc1 = yc1 > 0 ? yc1 : 0;
    xc0 = xc0 > 0 ? xc0 : 0;
    xc1 = xc1 > 0 ? xc1 : 0;
    const float fx = x - (float)x0;
    const float fy = y - (float)y0;
    const float omx = 1.0f - fx;
    const float omy = 1.0f - fy;
    const int row0 = w * yc0, row1 = w * yc1;
    float v = (omx * omy) * src[xc0 + row0];
    v = ((omy * fx) * src[xc1 + row0 + 1]) + v;
    v = ((omx * fy) * src[xc0 + row1]) + v;
    v = ((fy * fx) * src[xc1 + row1 + 1]) + v;
    return v;
}

// sub_180001BB0: lowest and highest radius of the frame as seen from the centre (int truncation).
static void RotBounds(int w, int h, float cx, float cy, int &low, int &high) {
    float v9 = 0.0f, v8 = 0.0f, v6 = 0.0f, v12 = 0.0f;
    const float fw = (float)w, fh = (float)h;
    if (cx < 0.0f) {
        v8 = fw - cx;
        v9 = -cx;
    } else if (cx >= fw) {
        v9 = cx - fw;
        v8 = cx;
    } else {
        v9 = 0.0f;
        v8 = (cx > (float)(w / 2)) ? cx : (fw - cx);
    }
    if (cy >= 0.0f) {
        if (cy < fh) {
            v6 = 0.0f;
            v12 = (cy <= (float)(h / 2)) ? (fh - cy) : cy;
        } else {
            v6 = cy - fh;
            v12 = cy;
        }
    } else {
        v6 = -cy;
        v12 = fh - cy;
    }
    low = (int)sqrtf((v6 * v6) + (v9 * v9));
    high = (int)sqrtf((v12 * v12) + (v8 * v8));
}

// sub_180001B10: frame pixel -> (radius, theta) in the rotated, ratio-scaled polar frame.
static void RotPixelMap(float x, float y, float cx, float cy, float cos_a, float sin_a, float ratio,
                        float &radius, float &theta) {
    const float dy = y - cy;
    const float dx = x - cx;
    const float a = ((cos_a * dy) - (sin_a * dx)) / ratio;
    const float b = (cos_a * dx) + (sin_a * dy);
    radius = sqrtf((a * a) + (b * b));
    theta = atan2f(a, b);
    if (theta < 0.0f) theta = (float)((double)theta + 6.2831853);
}

// sub_180001000: writer. Bilinear over (theta, radius) cells; colour un-premultiplied by the accumulated
// alpha; output alpha is the un-normalised weighted sum.
static void RotWrite(const float *P, int angles, float theta_idx, float radius_idx, float out[4]) {
    const int stride4 = 4 * angles;
    int v9 = (int)theta_idx;
    const float fa = theta_idx - (float)(int)theta_idx;
    int v10 = v9 + 1;
    const float fr = radius_idx - (float)(int)radius_idx;
    const int last = angles - 1;
    int v6 = 0;
    if (v9 >= 0) {
        if (v9 == last) v10 = 0;
        v6 = v10;
    } else {
        v9 = last;
    }
    const int rad = (int)radius_idx;
    const float *c0 = P + 4 * ((size_t)angles * (size_t)rad + (size_t)v9);
    const float *c1 = P + 4 * ((size_t)angles * (size_t)rad + (size_t)v6);
    const float *r0 = c0 + stride4;
    const float *r1 = c1 + stride4;
    float a = 0.0f, b = 0.0f, g = 0.0f, r = 0.0f;
    auto acc = [&](float wgt, const float *p) {
        const float al = wgt * p[3];
        a = al + a;
        b = (al * p[2]) + b;
        g = (al * p[1]) + g;
        r = (al * p[0]) + r;
    };
    acc((1.0f - fa) * (1.0f - fr), c0);
    acc((1.0f - fa) * fr, r0);
    acc((1.0f - fr) * fa, c1);
    acc(fr * fa, r1);
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = a;
    if (a != 0.0f) {
        const float inv = (float)(1.0 / (double)a);
        out[0] = r * inv;
        out[1] = g * inv;
        out[2] = inv * b;
    }
}

// sub_180001C90: scatter one cell along the angle axis. Outer (inner == false) walks forward in angle,
// inner walks backward. Targets accumulate premultiplied colour and weight; max goes to accw.
static void RotScatterCell(bool inner, int L, int t, int r, float avg, float R, float G, float B, float F,
                           int angles, int mode, int strength, const std::vector<float> &lut,
                           std::vector<float> &accum, std::vector<float> &accw) {
    int v = strength;
    if (mode == 1) v += L;
    else if (mode == 2) { if (v < L) v = L; }
    else if (mode == 3) v = L;
    if (v > 3000) v = 3000;
    const int reach = (int)((float)v * F);
    if (reach <= 0) return;
    const float step = (float)(30000 / reach);
    for (int k = 1; k < reach; ++k) {
        const int idx = (int)((float)k * step);
        const float weight = avg * lut[(size_t)idx];
        int tt = inner ? (t - k) % angles : (t + k) % angles;
        if (tt < 0) tt += angles;
        const size_t tc = (size_t)r * (size_t)angles + (size_t)tt;
        float *dst = &accum[tc * 4u];
        dst[0] = (weight * R) + dst[0];
        dst[1] = (weight * G) + dst[1];
        dst[2] = (weight * B) + dst[2];
        dst[3] = weight + dst[3];
        if (weight > accw[tc]) accw[tc] = weight;
    }
}

// Sine and cosine of one angle (binary: __libm_sse2_sincosf_ in the sincos table loop).
static void RotSinCos(float a, float &s, float &c) {
    s = sinf(a);
    c = cosf(a);
}

// Full rotation pipeline (binary sub_1800046B0). Output is w*h RGBA float.
static void RenderRotationBinary(const RotInputs &in, std::vector<float> &out) {
    const int w = in.w, h = in.h;
    out.assign((size_t)w * (size_t)h * 4u, 0.0f);
    if (w <= 0 || h <= 0 || !in.rgba || !in.factor_comb) return;

    // sub_180001AC0: step (a1[0]), radians per step (a1[1]), its reciprocal (a1[2]).
    const float q = in.quality_step > 0.0f ? in.quality_step : 0.2f;
    const float step_rad = q * 0.0174532925f;
    const float inv_step = (float)(1.0 / (double)step_rad);
    const double sc = 0.2 / (double)q;

    const float cos_a = cosf(in.angle_rad);
    const float sin_a = sinf(in.angle_rad);
    const float cx = (float)in.cx;
    const float cy = (float)in.cy;
    const float ratio = in.ratio;

    const int off_out = (int)((double)in.outer_offset * sc);
    const int off_in = (int)((double)in.inner_offset * sc);
    const int str_out = (int)((double)in.outer_strength * sc);
    const int str_in = (int)(sc * (double)in.inner_strength);
    const int fade_out = (int)(sc * (double)in.outer_fade);
    const int fade_in = (int)(sc * (double)in.inner_fade);

    std::vector<float> lut_scat_out, lut_scat_in, lut_fade_out, lut_fade_in;
    BuildLut(lut_scat_out, 30000);
    BuildLut(lut_scat_in, 30000);
    BuildLut(lut_fade_out, fade_out);
    BuildLut(lut_fade_in, fade_in);

    int low = 0, high = 0;
    RotBounds(w, h, cx, cy, low, high);
    int lo = (int)((float)low / ratio) - 2;
    if (lo < 0) lo = 0;
    high += 2;
    const int rows = (high - lo) + 1;
    const int angles = (int)(float)(360.0 / (double)q);
    if (rows <= 0 || angles <= 0) return;
    const size_t cells = (size_t)rows * (size_t)angles;

    // sub_1800046B0 sincos table: angle_t = (float)t * step_rad; entry = (cos, sin).
    std::vector<float> tab_cos((size_t)angles), tab_sin((size_t)angles);
    for (int t = 0; t < angles; ++t) {
        const float a = (float)t * step_rad;
        float s = 0.0f, c = 0.0f;
        RotSinCos(a, s, c);
        tab_sin[(size_t)t] = s;
        tab_cos[(size_t)t] = c;
    }

    std::vector<float> P(cells * 4u, 0.0f), Fc(cells, 0.0f), Fs(cells, 1.0f), avg(cells, 0.0f);
    std::vector<float> acc(cells * 4u, 0.0f), accw(cells, 0.0f);
    std::vector<unsigned char> mask(cells, 0);

    // Sampling: colour (P), validity mask, combined factor, size factor (1.0 when size is inactive).
    for (int r = 0; r < rows; ++r) {
        const float radius = (float)(r + lo);
        for (int t = 0; t < angles; ++t) {
            const size_t c = (size_t)r * (size_t)angles + (size_t)t;
            const float u = radius * tab_cos[(size_t)t];
            const float v = (radius * ratio) * tab_sin[(size_t)t];
            const float Y = (float)((cos_a * v) + (sin_a * u)) + cy;
            const float X = (float)((cos_a * u) - (sin_a * v)) + cx;
            float *p = &P[c * 4u];
            mask[c] = (unsigned char)(in.repeat ? RotSampleColourRep(in.rgba, w, h, X, Y, p)
                                               : RotSampleColourNR(in.rgba, w, h, X, Y, p));
            Fc[c] = in.repeat ? RotSampleFactorRep(in.factor_comb, w, h, X, Y)
                              : RotSampleFactorNR(in.factor_comb, w, h, X, Y);
            if (in.size_active && in.factor_size)
                Fs[c] = in.repeat ? RotSampleFactorRep(in.factor_size, w, h, X, Y)
                                  : RotSampleFactorNR(in.factor_size, w, h, X, Y);
            else
                Fs[c] = 1.0f;
        }
    }

    // sub_180002780: fade. Centre alpha and size factor drive a LUT-weighted alpha average.
    for (int r = 0; r < rows; ++r) {
        for (int t = 0; t < angles; ++t) {
            const size_t c = (size_t)r * (size_t)angles + (size_t)t;
            const float *p = &P[c * 4u];
            const float Fv = Fs[c];
            const float alpha_c = p[3];
            if (alpha_c == 0.0f || Fv == 0.0f) {
                acc[c * 4u] = acc[c * 4u + 1] = acc[c * 4u + 2] = acc[c * 4u + 3] = 0.0f;
                avg[c] = 0.0f;
                accw[c] = 0.0f;
                continue;
            }
            const float invF = (float)(1.0 / (double)Fv);
            const int n_out = (int)((float)fade_out * Fv);
            const int n_in = (int)((float)fade_in * Fv);
            float sum = alpha_c;
            float wsum = 1.0f;
            for (int k = 1; k < n_out; ++k) {
                const int li = (int)((float)k * invF);
                // Index == LUT length happens through float rounding; the binary reads the zeroed slot past the end.
                const float lw = li < (int)lut_fade_out.size() ? lut_fade_out[(size_t)li] : 0.0f;
                int tt = (t - k) % angles;
                if (tt < 0) tt += angles;
                // Binary quirk (sub_180002780): at t == 0 the first outer tap reads the previous ring's last cell
                // (flat index r*angles - 1); later taps wrap within the ring. Ring 0 reads the byte before the array.
                const float al = (t == 0 && k == 1)
                    ? (r > 0 ? P[((size_t)r * (size_t)angles - 1u) * 4u + 3u] : 0.0f)
                    : P[((size_t)r * (size_t)angles + (size_t)tt) * 4u + 3u];
                wsum = wsum + lw;
                sum = sum + (lw * al);
            }
            for (int k = 1; k < n_in; ++k) {
                const int li = (int)((float)k * invF);
                const float lw = li < (int)lut_fade_in.size() ? lut_fade_in[(size_t)li] : 0.0f;
                // Binary quirk (sub_180002780 inner taps): the walking pointer re-bases to cell 1 after a wrap, so tap
                // j = t + k reads flat cell r*angles + ((j - 1) % angles) + 1; j == angles reads the next ring's cell 0.
                const size_t flat = (size_t)r * (size_t)angles + (size_t)(((t + k - 1) % angles) + 1);
                const float al = flat < cells ? P[flat * 4u + 3u] : 0.0f;
                wsum = wsum + lw;
                sum = sum + (lw * al);
            }
            const float a = sum / wsum;
            acc[c * 4u] = a * p[0];
            acc[c * 4u + 1] = a * p[1];
            acc[c * 4u + 2] = a * p[2];
            acc[c * 4u + 3] = a;
            avg[c] = a;
            accw[c] = a;
        }
    }

    // sub_1800024C0: scatter. Per cell: outer (forward) then inner (backward); lengths scale with rows/2 / (r+1).
    const int half = rows / 2;
    for (int r = 0; r < rows; ++r) {
        const float invr = (float)(1.0 / (double)(float)(r + 1));
        const int L_out = (int)((float)(off_out * half) * invr);
        const int L_in = (int)((float)(off_in * half) * invr);
        for (int t = 0; t < angles; ++t) {
            const size_t c = (size_t)r * (size_t)angles + (size_t)t;
            const float a = avg[c];
            const float F = Fc[c];
            if (!mask[c] || a == 0.0f || F == 0.0f) continue;
            const float *p = &P[c * 4u];
            RotScatterCell(false, L_out, t, r, a, p[0], p[1], p[2], F, angles,
                           in.outer_mode, str_out, lut_scat_out, acc, accw);
            RotScatterCell(true, L_in, t, r, a, p[0], p[1], p[2], F, angles,
                           in.inner_mode, str_in, lut_scat_in, acc, accw);
        }
    }

    // Normalise: colour / accumulated alpha; alpha = accumulated weight; colour zero when the weight is zero.
    for (size_t c = 0; c < cells; ++c) {
        const float wgt = accw[c];
        float *p = &P[c * 4u];
        if (wgt == 0.0f) {
            p[0] = 0.0f;
            p[1] = 0.0f;
            p[2] = 0.0f;
        } else {
            const float *ac = &acc[c * 4u];
            p[0] = ac[0] / ac[3];
            p[1] = ac[1] / ac[3];
            p[2] = ac[2] / ac[3];
        }
        p[3] = wgt;
    }

    // Output: per frame pixel, polar coordinates and writer. The output pointer advances only on written pixels.
    for (int y = 0; y < h; ++y) {
        size_t wi = (size_t)y * (size_t)w;
        for (int x = 0; x < w; ++x) {
            float radius = 0.0f, theta = 0.0f;
            RotPixelMap((float)x, (float)y, cx, cy, cos_a, sin_a, ratio, radius, theta);
            float ti = theta * inv_step;
            if (ti >= (float)angles) ti = ti - (float)angles;
            const float ri = radius - (float)lo;
            if (ri >= 0.0f) {
                RotWrite(P.data(), angles, ti, ri, &out[wi * 4u]);
                ++wi;
            }
        }
    }
}

void RenderPolar(const PF_EffectWorld *input, PF_EffectWorld *output, int bitdepth,
                 const OLMRBParams &params, Image &image,
                 const std::vector<float> &factor_size, const std::vector<float> &factor_b, bool zoom,
                 std::vector<float> &destination) {
    if (!zoom) {
        // Rotation: binary sub_1800046B0 pipeline (RenderRotationBinary), inputs as the binary work struct.
        RotInputs in;
        in.w = image.width;
        in.h = image.height;
        in.rgba = image.rgba.data();
        in.factor_size = factor_size.data();
        in.factor_comb = factor_b.data();
        in.size_active = params.size_variation_active;
        in.cx = params.center_x;
        in.cy = params.center_y;
        in.ratio = params.ratio;
        in.angle_rad = params.angle_rad;
        in.quality_step = params.quality_step_degrees;
        in.outer_mode = (int)params.outer_offset_mode;
        in.outer_offset = (int)params.outer_offset;
        in.inner_mode = (int)params.inner_offset_mode;
        in.inner_offset = (int)params.inner_offset;
        in.outer_strength = (int)params.outer_strength;
        in.inner_strength = (int)params.inner_strength;
        in.outer_fade = (int)params.outer_fade;
        in.inner_fade = (int)params.inner_fade;
        in.repeat = params.repeat_border;
        RenderRotationBinary(in, destination);
        WriteWorld(output, bitdepth, destination, output->width, output->height, params.brightness);
        return;
    }
    const float cx = (float)params.center_x;
    const float cy = (float)params.center_y;
    const PolarBounds bounds = GetPolarBounds(params, image, cx, cy);
    if (bounds.rows <= 0 || bounds.angle_count <= 0) return;
    std::vector<float> polar;
    std::vector<float> polar_factor;
    std::vector<float> base;
    std::vector<float> weight_sum;
    std::vector<float> max_alpha;
    std::vector<float> polar_size;
    std::vector<unsigned char> polar_mask;
    FillPolar(image, factor_size, factor_b, params, bounds, cx, cy, zoom, polar, polar_size,
              polar_factor, polar_mask);

    std::vector<float> outer_fade_lut, inner_fade_lut;
    std::vector<float> outer_blur_lut, inner_blur_lut;
    int outer_strength = 0, inner_strength = 0;
    int outer_fade = 0, inner_fade = 0;
    if (zoom) {
        outer_strength = (int)((float)params.outer_strength * params.scale);
        inner_strength = (int)((float)params.inner_strength * params.scale);
        outer_fade = (int)((float)params.outer_fade * params.scale);
        inner_fade = (int)((float)params.inner_fade * params.scale);
        BuildLut(outer_blur_lut, outer_strength);
        BuildLut(inner_blur_lut, inner_strength);
        BuildLut(outer_fade_lut, outer_fade);
        BuildLut(inner_fade_lut, inner_fade);
    }

    // Binary zoom core (sub_18000A4D0 fade): the outer fade taps toward lower indices, the inner toward higher.
    FadePolar(bounds, zoom, outer_fade, inner_fade, outer_fade_lut, inner_fade_lut,
              polar, polar_size, base, weight_sum, max_alpha);
    ScatterZoom(bounds, outer_strength, inner_strength, outer_blur_lut,
                inner_blur_lut, polar, polar_mask, polar_factor, base, weight_sum, max_alpha);

    const size_t polar_count = (size_t)bounds.rows * (size_t)bounds.angle_count;
    NormalizePolar(polar_count, base, weight_sum, max_alpha);
    // The inverse map is evaluated on the input grid (binary sub_180005070 loops over the input world).
    destination.assign((size_t)image.width * (size_t)image.height * 4u, 0.0f);
    const float cos_angle = cosf(params.angle_rad);
    const float sin_angle = sinf(params.angle_rad);
    for (int y = 0; y < image.height; ++y) {
        const float py = (float)y;
        for (int x = 0; x < image.width; ++x) {
            const float px = (float)x;
            const float dx = px - cx;
            const float dy = py - cy;
            const float u = cos_angle * dx + sin_angle * dy;
            const float v = (cos_angle * dy - sin_angle * dx) / params.ratio;
            const float radius = sqrtf(u * u + v * v) - (float)bounds.low_radius;
            float theta = atan2f(v, u);
            // Binary (sub_180009BD0): theta + 6.2831853 evaluated in double, then rounded to float.
            if (theta < 0.0f) theta = (float)((double)theta + 6.2831853);
            const float angle_index = theta / bounds.angle_step;
            float sample[4];
            SamplePolarBilinear(bounds, zoom, angle_index, radius, base, sample);
            float *dst = &destination[((size_t)y * (size_t)image.width + (size_t)x) * 4u];
            memcpy(dst, sample, sizeof(sample));
        }
    }
    WriteWorld(output, bitdepth, destination, output->width, output->height, params.brightness);
}

template <typename Pixel, typename Channel>
PF_Err Kernel(PF_InData *in_data, PF_EffectWorld *input, PF_EffectWorld *output,
              int bitdepth, const OLMRBParams &checked_params, float divisor) {
    if (!in_data->utils || !in_data->utils->copy)
        return PF_Err_BAD_CALLBACK_PARAM;
    PF_Err err = in_data->utils->copy(in_data->effect_ref, input, output, nullptr, nullptr);
    if (err) return err;

    if (checked_params.outer_strength == 0 && checked_params.inner_strength == 0 &&
        checked_params.outer_offset == 0 && checked_params.inner_offset == 0 &&
        checked_params.outer_fade == 0 && checked_params.inner_fade == 0)
        return PF_Err_NONE;
    if (!input || !output || !input->data || !output->data || input->width <= 0 ||
        input->height <= 0)
        return PF_Err_BAD_CALLBACK_PARAM;

    try {
        OLMRBParams params = checked_params;
        params.scale = (float)in_data->downsample_x.num / (float)in_data->downsample_x.den;
        // Preserve the binary's float scale multiply after the point suite's double coordinates.
        params.center_x = (params.center_x - (double)input->origin_x) * (double)params.scale;
        params.center_y = (params.center_y - (double)input->origin_y) * (double)params.scale;
        params.thickness *= params.scale;

        Image image;
        if (bitdepth == 8) DecodeWorld<PF_Pixel, A_u_char>(input, divisor, image);
        else if (bitdepth == 16) DecodeWorld<PF_Pixel16, A_u_short>(input, divisor, image);
        else DecodeWorld32(input, image);

        const size_t count = (size_t)image.width * (size_t)image.height;
        std::vector<float> area_map(count, params.size_variation_active ? 0.0f : 1.0f);
        std::vector<unsigned char> mask(count, 0);
        float max_area = 1.0f;
        if (params.size_variation_active) {
            for (size_t i = 0; i < count; ++i)
                mask[i] = image.rgba[i * 4u + 3u] > 0.0f ? 1u : 0u;
            BuildComponents(image.width, image.height, mask, area_map, &max_area);
        }

        std::vector<float> factor_a(count, 1.0f);
        std::vector<float> factor_b(count, 1.0f);
        std::vector<float> layer_noise;
        NoiseField generated_noise;
        if (params.noise_type == OLMRB_NOISE_LAYER_MODE) {
            layer_noise.assign(count, 0.0f);
            if (params.has_noise_layer && params.noise_layer.data) {
                for (int y = 0; y < image.height; ++y) {
                    for (int x = 0; x < image.width; ++x) {
                        const int nx = x + input->origin_x - params.noise_layer.origin_x;
                        const int ny = y + input->origin_y - params.noise_layer.origin_y;
                        layer_noise[image.pixel(x, y)] = ReadNoisePixel(
                            params.noise_layer, nx, ny, bitdepth);
                    }
                }
            }
        } else {
            BuildNoiseField(generated_noise, image.width, image.height, params.thickness,
                            params.noise_offset_rad, params.seed);
        }

        // Binary sub_180006970 a5+136 loop: (area * float(1/max)) * sv + (1 - sv); 1/max is 1.0 when max <= 0.
        const float inv_max_area = max_area > 0.0f ? (float)(1.0 / (double)max_area) : 1.0f;
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                const size_t i = image.pixel(x, y);
                if (params.size_variation_active) {
                    factor_a[i] = (area_map[i] * inv_max_area) * params.size_variation +
                                  (1.0f - params.size_variation);
                } else {
                    factor_a[i] = 1.0f;
                }
                const float noise = params.noise_type == OLMRB_NOISE_LAYER_MODE
                    ? layer_noise[i]
                    : SampleNoiseField(generated_noise, (float)x, (float)y,
                                       params.noise_type == OLMRB_NOISE_SMOOTH,
                                       params.repeat_border);
                factor_b[i] = factor_a[i] * (params.noise_variation * noise +
                                             (1.0f - params.noise_variation));
            }
        }

        std::vector<float> rendered;
        RenderPolar(input, output, bitdepth, params, image, factor_a, factor_b,
                    params.blur_type == OLMRB_BLUR_ZOOM, rendered);
    } catch (const std::bad_alloc &) {
        return PF_Err_OUT_OF_MEMORY;
    } catch (...) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return PF_Err_NONE;
}

PF_Err SmartRender(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra) {
    PF_EffectWorld *input = nullptr;
    PF_EffectWorld *output = nullptr;
    const PF_Err input_err = extra->cb->checkout_layer_pixels(in_data->effect_ref,
                                                               OLMRB_INPUT, &input);
    const PF_Err output_err = extra->cb->checkout_output(in_data->effect_ref, &output);
    PF_Err err = input_err ? input_err : output_err;
    bool checked_out = input_err == PF_Err_NONE;
    if (!err && (!input || !output)) err = PF_Err_BAD_CALLBACK_PARAM;

    OLMRBParams params;
    if (!err) err = CheckoutParams(in_data, out_data, &params);
    if (!err) {
        switch (extra->input->bitdepth) {
        case 8: err = Kernel<PF_Pixel, A_u_char>(in_data, input, output, 8, params, 255.0f); break;
        case 16: err = Kernel<PF_Pixel16, A_u_short>(in_data, input, output, 16, params, 32768.0f); break;
        case 32: err = Kernel<PF_PixelFloat, PF_FpShort>(in_data, input, output, 32, params, 1.0f); break;
        default: err = PF_Err_BAD_CALLBACK_PARAM; break;
        }
    }
    if (checked_out) {
        const PF_Err checkin_err = extra->cb->checkin_layer_pixels(in_data->effect_ref, OLMRB_INPUT);
        if (!err) err = checkin_err;
    }
    return err;
}

} // namespace

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
        case PF_Cmd_UPDATE_PARAMS_UI:
            return UpdateParamsUI(in_data, out_data, params);
        case PF_Cmd_SMART_PRE_RENDER:
            return PreRender(in_data, (PF_PreRenderExtra *)extra);
        case PF_Cmd_SMART_RENDER:
            return SmartRender(in_data, out_data, (PF_SmartRenderExtra *)extra);
        case PF_Cmd_RENDER:
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

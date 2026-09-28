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

PF_Err ParamsSetup(PF_InData *in_data, PF_OutData *out_data) {
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Blur Type", 2, OLMRB_BLUR_ZOOM, "Zoom | Rotation",
                  PF_ParamFlag_SUPERVISE, OLMRB_ID_BLUR_TYPE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POINT("Center", 50, 50, FALSE, OLMRB_ID_CENTER);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Outer Blur", PF_ParamFlag_COLLAPSE_TWIRLY, OLMRB_ID_OUTER_GROUP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Strength", 0, 2000, 0, 2000, 0, OLMRB_ID_OUTER_STRENGTH);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Offset Mode", 3, OLMRB_OFFSET_ADD, "Add | Max | Override",
                  PF_ParamFlag_SUPERVISE, OLMRB_ID_OUTER_OFFSET_MODE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Offset", 0, 500, 0, 500, 0, OLMRB_ID_OUTER_OFFSET);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Edge Fade", 0, 100, 0, 100, 0, OLMRB_ID_OUTER_FADE);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OLMRB_ID_OUTER_GROUP_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Inner Blur", PF_ParamFlag_COLLAPSE_TWIRLY, OLMRB_ID_INNER_GROUP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Strength", 0, 2000, 0, 2000, 0, OLMRB_ID_INNER_STRENGTH);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Offset Mode", 3, OLMRB_OFFSET_ADD, "Add | Max | Override",
                  PF_ParamFlag_SUPERVISE, OLMRB_ID_INNER_OFFSET_MODE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Offset", 0, 500, 0, 500, 0, OLMRB_ID_INNER_OFFSET);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Edge Fade", 0, 100, 0, 100, 0, OLMRB_ID_INNER_FADE);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OLMRB_ID_INNER_GROUP_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Repeat Border", 1, PF_ParamFlag_NONE, OLMRB_ID_REPEAT_BORDER);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Ellipse", PF_ParamFlag_COLLAPSE_TWIRLY, OLMRB_ID_ELLIPSE_GROUP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDER("Ratio", 1, 5, 1, 5, AEFX_DEFAULT_CURVE_TOLERANCE, 1.0, 2,
                        PF_ValueDisplayFlag_NONE, false, OLMRB_ID_RATIO);

    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Angle", 0, OLMRB_ID_ANGLE);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OLMRB_ID_ELLIPSE_GROUP_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDER("Quality", 1, 50, 1, 50, AEFX_DEFAULT_CURVE_TOLERANCE, 5.0, 2,
                        PF_ValueDisplayFlag_NONE, false, OLMRB_ID_QUALITY);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDER("Brightness Gain", 0, 2, 0, 2, AEFX_DEFAULT_CURVE_TOLERANCE, 1.0, 1,
                        PF_ValueDisplayFlag_NONE, false, OLMRB_ID_BRIGHTNESS);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDER("Size Variation", 0, 100, 0, 100, AEFX_DEFAULT_CURVE_TOLERANCE, 0.0, 1,
                        PF_ValueDisplayFlag_NONE, false, OLMRB_ID_SIZE_VARIATION);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Noise Parameters", PF_ParamFlag_COLLAPSE_TWIRLY, OLMRB_ID_NOISE_GROUP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDER("Noise Variation", 0, 100, 0, 100, AEFX_DEFAULT_CURVE_TOLERANCE, 0.0, 1,
                        PF_ValueDisplayFlag_NONE, false, OLMRB_ID_NOISE_VARIATION);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Noise Type", 3, OLMRB_NOISE_SMOOTH, "Smooth | Block | Layer",
                  PF_ParamFlag_SUPERVISE, OLMRB_ID_NOISE_TYPE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Noise Layer", 0, OLMRB_ID_NOISE_LAYER);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Seed", 1, 1000, 1, 1000, 1, OLMRB_ID_SEED);

    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Offset", 0, OLMRB_ID_NOISE_OFFSET);

    AEFX_CLR_STRUCT(def);
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
        p->quality_step_degrees = quality > 0.0f ? 1.0f / quality : 0.2f;
    });
    OLMRB_READ(OLMRB_BRIGHTNESS, p->brightness = (float)def.u.fs_d.value);
    OLMRB_READ(OLMRB_SIZE_VARIATION, {
        p->size_variation = (float)def.u.fs_d.value / 100.0f;
        p->size_variation_active = p->size_variation > 0.0001f;
    });
    OLMRB_READ(OLMRB_NOISE_VARIATION,
               p->noise_variation = (float)def.u.fs_d.value / 100.0f);
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
                        PF_InData *in_data, PF_ParamIndex position, PF_Boolean disabled) {
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    def.ui_flags = disabled ? PF_PUI_DISABLED : PF_PUI_NONE;
    return suite->PF_UpdateParamUI(in_data->effect_ref, position, &def);
}

PF_Err UpdateParamsUI(PF_InData *in_data, PF_OutData *out_data) {
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
                           rotation && outer_mode == OLMRB_OFFSET_OVERRIDE);
    if (err) return err;
    err = SetParamDisabled(param_utils, in_data, OLMRB_OUTER_OFFSET_MODE, !rotation);
    if (err) return err;
    err = SetParamDisabled(param_utils, in_data, OLMRB_OUTER_OFFSET, !rotation);
    if (err) return err;
    err = SetParamDisabled(param_utils, in_data, OLMRB_INNER_STRENGTH,
                           rotation && inner_mode == OLMRB_OFFSET_OVERRIDE);
    if (err) return err;
    err = SetParamDisabled(param_utils, in_data, OLMRB_INNER_OFFSET_MODE, !rotation);
    if (err) return err;
    err = SetParamDisabled(param_utils, in_data, OLMRB_INNER_OFFSET, !rotation);
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
    std::vector<float> table;
    std::vector<float> values;
};

void BuildNoiseField(NoiseField &field, int width, int height, float cell,
                     float offset, int seed) {
    field.cell = cell;
    const float inv_cell = 1.0f / cell;
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
    float gx = x / field.cell;
    float gy = y / field.cell;
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
    return ((1.0f - wy) * wx) * row[1] +
           ((1.0f - wx) * (1.0f - wy)) * row[0] +
           (wy * wx) * row[field.width + 1] +
           ((1.0f - wx) * wy) * row[field.width];
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
    for (int y = 0; y < image.height; ++y) {
        const char *row = (const char *)world->data + (size_t)y * (size_t)world->rowbytes;
        const Pixel *pixels = (const Pixel *)row;
        for (int x = 0; x < image.width; ++x) {
            const Pixel &src = pixels[x];
            float *dst = &image.rgba[image.pixel(x, y) * 4u];
            dst[0] = (float)(Channel)src.red / divisor;
            dst[1] = (float)(Channel)src.green / divisor;
            dst[2] = (float)(Channel)src.blue / divisor;
            dst[3] = (float)(Channel)src.alpha / divisor;
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

void SampleRgba(const Image &image, float x, float y, bool repeat, float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (image.width <= 0 || image.height <= 0) return;
    const int x0 = (int)x;
    const int y0 = (int)y;
    if (x0 <= -2 || y0 <= -2 || x0 >= image.width || y0 >= image.height) {
        return;
    }

    // The original samplers use truncation for integer conversion and alpha-weighted color taps.
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
    if (coverage != 0.0f) out[3] = alpha / coverage;
    if (alpha != 0.0f) {
        out[0] = rgb[0] / alpha;
        out[1] = rgb[1] / alpha;
        out[2] = rgb[2] / alpha;
    }
}

float SampleScalar(const std::vector<float> &values, int width, int height,
                   float x, float y, bool repeat) {
    if (width <= 0 || height <= 0) return 0.0f;
    const int x0 = (int)x;
    const int y0 = (int)y;
    if (x0 <= -2 || y0 <= -2 || x0 >= width || y0 >= height) {
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

void FillPolar(const Image &image, const std::vector<float> &factor_b, const OLMRBParams &p,
               const PolarBounds &bounds, float cx, float cy, bool zoom,
               std::vector<float> &polar, std::vector<float> &polar_factor) {
    const size_t count = (size_t)bounds.rows * (size_t)bounds.angle_count;
    polar.assign(count * 4u, 0.0f);
    polar_factor.assign(count, 0.0f);
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
            const float radial_y = (radius * st) * p.ratio;
            const float x = cx + ca * radial_x - sa * radial_y;
            const float y = cy + sa * radial_x + ca * radial_y;
            const size_t i = zoom ? ZoomIndex(bounds, outer, inner)
                                  : RotationIndex(bounds, inner, outer);
            SampleRgba(image, x, y, p.repeat_border, &polar[i * 4u]);
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
                const int lut_i = (int)((float)k / f);
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
                const int lut_i = (int)((float)k / f);
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
    const float inv_factor = 1.0f / factor;
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
                 const std::vector<float> &factor, std::vector<float> &base,
                 std::vector<float> &weight_sum, std::vector<float> &max_alpha) {
    for (int angle = 0; angle < bounds.angle_count; ++angle) {
        for (int radius = 0; radius < bounds.rows; ++radius) {
            const size_t i = ZoomIndex(bounds, angle, radius);
            const float *src = &source[i * 4u];
            const float center_alpha = base[i * 4u + 3u];
            ScatterZoomDirection(bounds, angle, radius, inner_strength, factor[i], true,
                                 inner_lut, src, center_alpha, base, weight_sum, max_alpha);
            ScatterZoomDirection(bounds, angle, radius, outer_strength, factor[i], false,
                                 outer_lut, src, center_alpha, base, weight_sum, max_alpha);
        }
    }
}

int CombineOffsetLength(int strength, int row_offset, int mode) {
    int length = strength;
    if (mode == OLMRB_OFFSET_ADD) length += row_offset;
    else if (mode == OLMRB_OFFSET_MAX) length = std::max(strength, row_offset);
    else if (mode == OLMRB_OFFSET_OVERRIDE) length = row_offset;
    return std::min(3000, std::max(0, length));
}

void ScatterRotationDirection(const PolarBounds &bounds, int radius, int angle, int length,
                              float factor, bool toward_lower, const std::vector<float> &lut,
                              const float source[4], float center_alpha, std::vector<float> &dst,
                              std::vector<float> &weight_sum, std::vector<float> &max_alpha) {
    if (length <= 0 || factor <= 0.0f || center_alpha == 0.0f) return;
    const int reach = (int)((float)length * factor);
    if (reach <= 1) return;
    const int step = 30000 / reach;
    for (int k = 1; k < reach; ++k) {
        const int lut_i = (int)((float)k * (float)step);
        if (lut_i < 0 || (size_t)lut_i >= lut.size()) break;
        int target_angle = toward_lower ? angle - k : angle + k;
        target_angle %= bounds.angle_count;
        if (target_angle < 0) target_angle += bounds.angle_count;
        const size_t target = RotationIndex(bounds, radius, target_angle);
        const float w = center_alpha * lut[(size_t)lut_i];
        float *pixel = &dst[target * 4u];
        pixel[0] += w * source[0];
        pixel[1] += w * source[1];
        pixel[2] += w * source[2];
        weight_sum[target] += w;
        max_alpha[target] = std::max(max_alpha[target], w);
    }
}

void ScatterRotation(const PolarBounds &bounds, const OLMRBParams &p, int outer_strength,
                     int inner_strength, int outer_offset, int inner_offset,
                     const std::vector<float> &outer_lut, const std::vector<float> &inner_lut,
                     const std::vector<float> &source, const std::vector<float> &factor,
                     std::vector<float> &base, std::vector<float> &weight_sum,
                     std::vector<float> &max_alpha) {
    const int half_rows = bounds.rows / 2;
    for (int radius = 0; radius < bounds.rows; ++radius) {
        const float row_factor = 1.0f / (float)(radius + 1);
        const int row_outer_offset = (int)((float)(outer_offset * half_rows) * row_factor);
        const int row_inner_offset = (int)((float)(inner_offset * half_rows) * row_factor);
        const int outer_length = CombineOffsetLength(outer_strength, row_outer_offset,
                                                     (int)p.outer_offset_mode);
        const int inner_length = CombineOffsetLength(inner_strength, row_inner_offset,
                                                     (int)p.inner_offset_mode);
        for (int angle = 0; angle < bounds.angle_count; ++angle) {
            const size_t i = RotationIndex(bounds, radius, angle);
            const float *src = &source[i * 4u];
            const float center_alpha = base[i * 4u + 3u];
            ScatterRotationDirection(bounds, radius, angle, inner_length, factor[i], true,
                                     inner_lut, src, center_alpha, base, weight_sum, max_alpha);
            ScatterRotationDirection(bounds, radius, angle, outer_length, factor[i], false,
                                     outer_lut, src, center_alpha, base, weight_sum, max_alpha);
        }
    }
}

void NormalizePolar(size_t count, std::vector<float> &rgba,
                    const std::vector<float> &weight_sum,
                    const std::vector<float> &max_alpha) {
    for (size_t i = 0; i < count; ++i) {
        const float weight = weight_sum[i];
        float *pixel = &rgba[i * 4u];
        if (weight > 0.0f) {
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
    for (int dr = 0; dr <= 1; ++dr) {
        const int rr = dr ? r1 : r0;
        const float wr = dr ? fr : 1.0f - fr;
        for (int da = 0; da <= 1; ++da) {
            const int aa = da ? a1 : a0;
            const float wa = da ? fa : 1.0f - fa;
            const size_t i = zoom ? ZoomIndex(bounds, aa, rr) : RotationIndex(bounds, rr, aa);
            const float w = wr * wa * polar[i * 4u + 3u];
            alpha_sum += w;
            rgb[0] += w * polar[i * 4u];
            rgb[1] += w * polar[i * 4u + 1u];
            rgb[2] += w * polar[i * 4u + 2u];
        }
    }
    if (alpha_sum != 0.0f) {
        out[0] = rgb[0] / alpha_sum;
        out[1] = rgb[1] / alpha_sum;
        out[2] = rgb[2] / alpha_sum;
    }
    out[3] = alpha_sum;
}

void WriteWorld(PF_EffectWorld *output, int bitdepth, const std::vector<float> &rgba,
                int width, int height, float brightness) {
    for (int y = 0; y < output->height; ++y) {
        char *row = (char *)output->data + (size_t)y * (size_t)output->rowbytes;
        for (int x = 0; x < output->width; ++x) {
            const float *src = nullptr;
            if (x < width && y < height)
                src = &rgba[((size_t)y * (size_t)width + (size_t)x) * 4u];
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

void RenderPolar(const PF_EffectWorld *input, PF_EffectWorld *output, int bitdepth,
                 const OLMRBParams &params, Image &image,
                 const std::vector<float> &factor_b, bool zoom,
                 std::vector<float> &destination) {
    const float cx = (float)params.center_x;
    const float cy = (float)params.center_y;
    const PolarBounds bounds = GetPolarBounds(params, image, cx, cy);
    if (bounds.rows <= 0 || bounds.angle_count <= 0) return;
    std::vector<float> polar;
    std::vector<float> polar_factor;
    std::vector<float> base;
    std::vector<float> weight_sum;
    std::vector<float> max_alpha;
    FillPolar(image, factor_b, params, bounds, cx, cy, zoom, polar, polar_factor);

    std::vector<float> outer_fade_lut, inner_fade_lut;
    std::vector<float> outer_blur_lut, inner_blur_lut;
    int outer_strength = 0, inner_strength = 0;
    int outer_fade = 0, inner_fade = 0;
    int outer_offset = 0, inner_offset = 0;
    if (zoom) {
        outer_strength = (int)((float)params.outer_strength * params.scale);
        inner_strength = (int)((float)params.inner_strength * params.scale);
        outer_fade = (int)((float)params.outer_fade * params.scale);
        inner_fade = (int)((float)params.inner_fade * params.scale);
        BuildLut(outer_blur_lut, outer_strength);
        BuildLut(inner_blur_lut, inner_strength);
        BuildLut(outer_fade_lut, outer_fade);
        BuildLut(inner_fade_lut, inner_fade);
    } else {
        const double scale = 0.2 / (double)params.quality_step_degrees;
        outer_strength = (int)(scale * (double)params.outer_strength);
        inner_strength = (int)(scale * (double)params.inner_strength);
        outer_offset = (int)(scale * (double)params.outer_offset);
        inner_offset = (int)(scale * (double)params.inner_offset);
        outer_fade = (int)(scale * (double)params.outer_fade);
        inner_fade = (int)(scale * (double)params.inner_fade);
        BuildLut(outer_blur_lut, 30000);
        BuildLut(inner_blur_lut, 30000);
        BuildLut(outer_fade_lut, outer_fade);
        BuildLut(inner_fade_lut, inner_fade);
    }

    // Verified in the binary (sub_18000A4D0 zoom, sub_180002780 rotation): in
    // BOTH modes the outer fade taps toward lower indices (radius/angle) and
    // the inner fade toward higher — the opposite of the strength scatter.
    const int low_fade = outer_fade;
    const int high_fade = inner_fade;
    const std::vector<float> &low_fade_lut = outer_fade_lut;
    const std::vector<float> &high_fade_lut = inner_fade_lut;
    FadePolar(bounds, zoom, low_fade, high_fade, low_fade_lut, high_fade_lut,
              polar, polar_factor, base, weight_sum, max_alpha);
    if (zoom) {
        ScatterZoom(bounds, outer_strength, inner_strength, outer_blur_lut,
                    inner_blur_lut, polar, polar_factor, base, weight_sum, max_alpha);
    } else {
        ScatterRotation(bounds, params, outer_strength, inner_strength, outer_offset,
                        inner_offset, outer_blur_lut, inner_blur_lut, polar,
                        polar_factor, base, weight_sum, max_alpha);
    }

    const size_t polar_count = (size_t)bounds.rows * (size_t)bounds.angle_count;
    NormalizePolar(polar_count, base, weight_sum, max_alpha);
    destination.assign((size_t)output->width * (size_t)output->height * 4u, 0.0f);
    const float cos_angle = cosf(params.angle_rad);
    const float sin_angle = sinf(params.angle_rad);
    for (int y = 0; y < output->height; ++y) {
        const float py = (float)(output->origin_y + y - input->origin_y);
        for (int x = 0; x < output->width; ++x) {
            const float px = (float)(output->origin_x + x - input->origin_x);
            const float dx = px - cx;
            const float dy = py - cy;
            const float u = cos_angle * dx + sin_angle * dy;
            const float v = (cos_angle * dy - sin_angle * dx) / params.ratio;
            const float radius = sqrtf(u * u + v * v) - (float)bounds.low_radius;
            float theta = atan2f(v, u);
            if (theta < 0.0f) theta += 2.0f * kPi;
            const float angle_index = theta / bounds.angle_step;
            float sample[4];
            SamplePolarBilinear(bounds, zoom, angle_index, radius, base, sample);
            float *dst = &destination[((size_t)y * (size_t)output->width + (size_t)x) * 4u];
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

        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                const size_t i = image.pixel(x, y);
                if (params.size_variation_active && max_area > 0.0f) {
                    factor_a[i] = (area_map[i] / max_area) * params.size_variation +
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
        RenderPolar(input, output, bitdepth, params, image, factor_b,
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
            return UpdateParamsUI(in_data, out_data);
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

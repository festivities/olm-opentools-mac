/* OLMColorKey: macOS SmartRender port of the Windows OLM Color Key 2.3.1.
 * Every kernel, conversion, distance metric, chamfer/EDT and blur profile
 * below follows the decompiled Windows binary (entry 0x180010010,
 * setup 0x180001000, checkout 0x18000A370, UI 0x180001A50, pre-render
 * 0x18000B560, smart render 0x1800018E0, kernels 0x180009440/8F90/98F0,
 * keyers 0x180001E00/29D0/35D0 (16-bit invert 0x1800035B0),
 * distances 0x180004190-0x180004910,
 * conversions 0x180009DA0/9EE0/A120, band mask 0x180008A60-8DD0,
 * chamfers 0x1800066F0/58A0 + per-depth twins, FH EDT 0x18000A710/A920,
 * blur 0x1800085A0-0x180008930 + profiles 0x1800049A0-0x1800056F0). */

#include "OLMColorKey.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

namespace {

constexpr float kPiF = 3.1415927f;            // binary blur-profile constant
constexpr float kHalfPi = 1.57079632679485f;  // binary blur-profile constant
constexpr float kAroundStep = 1.5707964f;     // binary Around-profile constant
constexpr float kClampDistance = 4000.0f;     // chamfer clamp (dword_18001F754)
constexpr float kLab76ShiftA = 133.037f;
constexpr float kLab76ShiftB = 163.48801f;
constexpr float kDegPerRad = 57.295776f;

static_assert(PF_VERSION(OLMCK_MAJOR_VERSION, OLMCK_MINOR_VERSION,
                          OLMCK_BUG_VERSION, PF_Stage_DEVELOP, 0) == 1148928,
              "ColorKey version must match its PiPL");

AEGP_PluginID g_aegp_id = 0;

PF_Err About(PF_InData *in_data, PF_OutData *out_data) {
    (void)in_data;
    PF_SPRINTF(out_data->return_msg, "%s", OLMCK_ABOUT);
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData *in_data, PF_OutData *out_data) {
    out_data->my_version = PF_VERSION(OLMCK_MAJOR_VERSION, OLMCK_MINOR_VERSION,
                                      OLMCK_BUG_VERSION, PF_Stage_DEVELOP, 0);
    // Binary raw out_flags are 0x02000440 (USE_OUTPUT_EXTENT | PIX_INDEPENDENT |
    // DEEP_COLOR_AWARE). The port adds PF_OutFlag_SEND_UPDATE_PARAMS_UI
    // (-> 0x06000440): per SDK AE_Effect.h that flag is required to receive
    // PF_Cmd_UPDATE_PARAMS_UI, which this effect needs for its parameter
    // visibility logic. Documented deviation from the binary.
    out_data->out_flags = PF_OutFlag_USE_OUTPUT_EXTENT | PF_OutFlag_PIX_INDEPENDENT |
                          PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_SEND_UPDATE_PARAMS_UI;
    // 0x08001400 = SUPPORTS_SMART_RENDER | FLOAT_COLOR_AWARE |
    //              SUPPORTS_THREADED_RENDERING (binary-exact; notably without
    //              PARAM_GROUP_START_COLLAPSED).
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER |
                           PF_OutFlag2_FLOAT_COLOR_AWARE |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    if (in_data->pica_basicP) {
        AEGP_SuiteHandler suites(in_data->pica_basicP);
        suites.UtilitySuite3()->AEGP_RegisterWithAEGP((AEGP_GlobalRefcon)0, OLMCK_NAME,
                                                      &g_aegp_id);
    }
    return PF_Err_NONE;
}

PF_Err ParamsSetup(PF_InData *in_data, PF_OutData *out_data) {
    PF_ParamDef def;
    PF_Err priv_err = PF_Err_NONE;

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Color Keep", 0, PF_ParamFlag_NONE, OLMCK_ID_COLOR_KEEP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Threshold", 0.0, 1.0, 0.0, 1.0, 0.0, 4,
                         PF_ValueDisplayFlag_NONE, PF_ParamFlag_COLLAPSE_TWIRLY,
                         OLMCK_ID_THRESHOLD);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Threshold Parameters", PF_ParamFlag_NONE, OLMCK_ID_THRESHOLD_GROUP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Premultiplied Color", 0, PF_ParamFlag_NONE, OLMCK_ID_PREMULTIPLIED);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Color Space", 6, OLMCK_SPACE_RGB, "RGB|HSV|Lab76|Lab94|YUV|YCrCb",
                  PF_ParamFlag_NONE, OLMCK_ID_COLOR_SPACE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Force Lower Precision", 3, OLMCK_PREC_FULL, "Full|16bit|8bit",
                  PF_ParamFlag_NONE, OLMCK_ID_FORCE_PRECISION);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Per Color", 0, PF_ParamFlag_NONE, OLMCK_ID_PER_COLOR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Per Component", 0, PF_ParamFlag_NONE, OLMCK_ID_PER_COMPONENT);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Threshold(R,H,L,Y,Y)", 0.0, 1.0, 0.0, 1.0, 0.0, 4,
                         PF_ValueDisplayFlag_NONE, PF_ParamFlag_COLLAPSE_TWIRLY,
                         OLMCK_ID_THRESHOLD_R);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Threshold(G,S,a,U,Cr)", 0.0, 1.0, 0.0, 1.0, 0.0, 4,
                         PF_ValueDisplayFlag_NONE, PF_ParamFlag_COLLAPSE_TWIRLY,
                         OLMCK_ID_THRESHOLD_G);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Threshold(B,V,b,V,Cb)", 0.0, 1.0, 0.0, 1.0, 0.0, 4,
                         PF_ValueDisplayFlag_NONE, PF_ParamFlag_COLLAPSE_TWIRLY,
                         OLMCK_ID_THRESHOLD_B);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OLMCK_ID_THRESHOLD_GROUP_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Edge Thin", PF_ParamFlag_NONE, OLMCK_ID_THIN_GROUP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Amount", -4000, 4000, -100, 100, 0, OLMCK_ID_THIN_AMOUNT);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Distance Type", 3, OLMCK_DIST_BOX, "Box|Approximate|Euclidean",
                  PF_ParamFlag_NONE, OLMCK_ID_THIN_DISTANCE);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OLMCK_ID_THIN_GROUP_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Edge Blur", PF_ParamFlag_NONE, OLMCK_ID_BLUR_GROUP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Amount", 0.0, 4000.0, 0.0, 100.0, 0.0, 1,
                         PF_ValueDisplayFlag_NONE, PF_ParamFlag_NONE,
                         OLMCK_ID_BLUR_AMOUNT);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Distance Type", 3, OLMCK_DIST_BOX, "Box|Approximate|Euclidean",
                  PF_ParamFlag_NONE, OLMCK_ID_BLUR_DISTANCE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Direction", 3, OLMCK_DIR_AROUND, "Inside | Around | Outside",
                  PF_ParamFlag_NONE, OLMCK_ID_BLUR_DIRECTION);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OLMCK_ID_BLUR_GROUP_END);

    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_SUPERVISE;
    PF_ADD_SLIDER("Number of Colors", 0, OLMCK_MAX_COLORS, 0, 30, 1, OLMCK_ID_COLOR_COUNT);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Enable Replace", 0, PF_ParamFlag_SUPERVISE, OLMCK_ID_ENABLE_REPLACE);

    char name[64];
    for (int n = 0; n < OLMCK_MAX_COLORS; ++n) {
        PF_SPRINTF(name, "Use Color %d", n + 1);
        AEFX_CLR_STRUCT(def);
        PF_ADD_CHECKBOXX(name, 1, PF_ParamFlag_NONE, OLMCK_ID_SLOT_USE_COLOR + 3 * n);

        PF_SPRINTF(name, "Use Replace Color %d", n + 1);
        AEFX_CLR_STRUCT(def);
        PF_ADD_CHECKBOXX(name, 0, PF_ParamFlag_NONE, OLMCK_ID_SLOT_USE_REPLACE + 3 * n);

        PF_SPRINTF(name, "Color %d", n + 1);
        AEFX_CLR_STRUCT(def);
        def.flags = PF_ParamFlag_COLLAPSE_TWIRLY;
        PF_ADD_COLOR(name, 0, 0, 0, OLMCK_ID_SLOT_COLOR + 5 * n);

        PF_SPRINTF(name, "Replace Color %d", n + 1);
        AEFX_CLR_STRUCT(def);
        def.flags = PF_ParamFlag_COLLAPSE_TWIRLY;
        PF_ADD_COLOR(name, 0, 0, 0, OLMCK_ID_SLOT_REPLACE_COLOR + 3 * n);

        PF_SPRINTF(name, "Threshold %d", n + 1);
        AEFX_CLR_STRUCT(def);
        PF_ADD_FLOAT_SLIDERX(name, 0.0, 1.0, 0.0, 1.0, 0.0, 4,
                             PF_ValueDisplayFlag_NONE, PF_ParamFlag_COLLAPSE_TWIRLY,
                             OLMCK_ID_SLOT_THRESHOLD + 5 * n);

        PF_SPRINTF(name, "Threshold(R,H,L,Y,Y) %d", n + 1);
        AEFX_CLR_STRUCT(def);
        PF_ADD_FLOAT_SLIDERX(name, 0.0, 1.0, 0.0, 1.0, 0.0, 4,
                             PF_ValueDisplayFlag_NONE, PF_ParamFlag_COLLAPSE_TWIRLY,
                             OLMCK_ID_SLOT_THRESHOLD_R + 5 * n);

        PF_SPRINTF(name, "Threshold(G,S,a,U,Cr) %d", n + 1);
        AEFX_CLR_STRUCT(def);
        PF_ADD_FLOAT_SLIDERX(name, 0.0, 1.0, 0.0, 1.0, 0.0, 4,
                             PF_ValueDisplayFlag_NONE, PF_ParamFlag_COLLAPSE_TWIRLY,
                             OLMCK_ID_SLOT_THRESHOLD_G + 5 * n);

        PF_SPRINTF(name, "Threshold(B,V,b,V,Cb) %d", n + 1);
        AEFX_CLR_STRUCT(def);
        PF_ADD_FLOAT_SLIDERX(name, 0.0, 1.0, 0.0, 1.0, 0.0, 4,
                             PF_ValueDisplayFlag_NONE, PF_ParamFlag_COLLAPSE_TWIRLY,
                             OLMCK_ID_SLOT_THRESHOLD_B + 5 * n);
    }
    (void)priv_err;

    out_data->num_params = OLMCK_NUM_PARAMS;
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

float EpsilonForDepth(A_long force_precision, A_long bitdepth) {
    // Float bit patterns from the binary checkout tail (0x18000A370):
    //   1/510 = 0x3B008081, 1/65536 = 0x37800000, 1e-6 = 0x358637BD.
    // Force 16bit never upgrades an 8-bit render; Force 8bit is always 1/510.
    uint32_t bits;
    if (force_precision == OLMCK_PREC_8) {
        bits = 0x3B008081u;
    } else if (force_precision == OLMCK_PREC_16) {
        bits = bitdepth == 8 ? 0x3B008081u : 0x37800000u;
    } else if (bitdepth == 8) {
        bits = 0x3B008081u;
    } else if (bitdepth == 16) {
        bits = 0x37800000u;
    } else {
        bits = 0x358637BDu;
    }
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

PF_Err CheckoutParams(PF_InData *in_data, PF_OutData *out_data, A_long bitdepth,
                      OLMCKParams *p) {
    std::memset(p, 0, sizeof(*p));
    PF_Err err;

#define OLMCK_READ(POS, BODY) \
    do { \
        err = checkout_value(in_data, (POS), [&](const PF_ParamDef &def) { BODY; }); \
        if (err) return err; \
    } while (0)

    OLMCK_READ(OLMCK_COLOR_KEEP, p->keep = def.u.bd.value != 0);
    OLMCK_READ(OLMCK_PREMULTIPLIED, p->premultiplied = def.u.bd.value != 0);
    OLMCK_READ(OLMCK_THIN_AMOUNT, p->thin_amount = def.u.sd.value);
    OLMCK_READ(OLMCK_THIN_DISTANCE, p->thin_distance = def.u.pd.value);
    OLMCK_READ(OLMCK_BLUR_AMOUNT, p->blur_amount = (float)def.u.fs_d.value);
    OLMCK_READ(OLMCK_BLUR_DISTANCE, p->blur_distance = def.u.pd.value);
    OLMCK_READ(OLMCK_BLUR_DIRECTION, p->blur_direction = def.u.pd.value);
    OLMCK_READ(OLMCK_COLOR_SPACE, p->color_space = def.u.pd.value);
    OLMCK_READ(OLMCK_PER_COLOR, p->per_color = def.u.bd.value != 0);
    OLMCK_READ(OLMCK_PER_COMPONENT, p->per_component = def.u.bd.value != 0);
    OLMCK_READ(OLMCK_THRESHOLD, p->global_threshold = (float)def.u.fs_d.value);
    OLMCK_READ(OLMCK_THRESHOLD_R, p->threshold_rgb[0] = (float)def.u.fs_d.value);
    OLMCK_READ(OLMCK_THRESHOLD_G, p->threshold_rgb[1] = (float)def.u.fs_d.value);
    OLMCK_READ(OLMCK_THRESHOLD_B, p->threshold_rgb[2] = (float)def.u.fs_d.value);
    OLMCK_READ(OLMCK_COLOR_COUNT, p->count = def.u.sd.value);
    OLMCK_READ(OLMCK_FORCE_PRECISION, p->force_precision = def.u.pd.value);
    OLMCK_READ(OLMCK_ENABLE_REPLACE, p->enable_replace = def.u.bd.value != 0);

    // Explicit rejection before the slot loop: with a count outside 0..25 the
    // slot parameters do not exist, and checking them out (even if a host
    // permitted it) would run past the arrays.
    if (p->count < 0 || p->count > OLMCK_MAX_COLORS) return PF_Err_BAD_CALLBACK_PARAM;

    // The binary checks out every enabled slot; with count > 25 the slot
    // parameters do not exist and checkout fails, rejecting the render
    // (0x18000A370 relies on the same failing id lookup).
    for (int n = 0; n < (int)p->count; ++n) {
        const int base = OLMCK_SLOT_BASE + n * 8;
        AEFX_SuiteScoper<PF_ColorParamSuite1> color_suite(
            in_data, kPFColorParamSuite, kPFColorParamSuiteVersion1, out_data);
        PF_ParamDef def;
        AEFX_CLR_STRUCT(def);
        err = PF_CHECKOUT_PARAM(in_data, base + 2, in_data->current_time, in_data->time_step,
                                in_data->time_scale, &def);
        if (err) return err;
        err = color_suite->PF_GetFloatingPointColorFromColorDef(in_data->effect_ref, &def,
                                                                &p->key_colors[n]);
        const PF_Err checkin_err = PF_CHECKIN_PARAM(in_data, &def);
        if (err) return err;
        if (checkin_err) return checkin_err;

        OLMCK_READ(base + 0, p->use_color[n] = def.u.bd.value != 0);
        OLMCK_READ(base + 1, p->use_replace[n] = def.u.bd.value != 0);

        AEFX_CLR_STRUCT(def);
        err = PF_CHECKOUT_PARAM(in_data, base + 3, in_data->current_time, in_data->time_step,
                                in_data->time_scale, &def);
        if (err) return err;
        err = color_suite->PF_GetFloatingPointColorFromColorDef(in_data->effect_ref, &def,
                                                                &p->replace_colors[n]);
        const PF_Err checkin_err2 = PF_CHECKIN_PARAM(in_data, &def);
        if (err) return err;
        if (checkin_err2) return checkin_err2;

        OLMCK_READ(base + 4, p->thresholds[n] = (float)def.u.fs_d.value);
        OLMCK_READ(base + 5, p->threshold_rgbs[n][0] = (float)def.u.fs_d.value);
        OLMCK_READ(base + 6, p->threshold_rgbs[n][1] = (float)def.u.fs_d.value);
        OLMCK_READ(base + 7, p->threshold_rgbs[n][2] = (float)def.u.fs_d.value);
    }

#undef OLMCK_READ

    p->bitdepth = bitdepth;
    p->epsilon = EpsilonForDepth(p->force_precision, bitdepth);
    return PF_Err_NONE;
}

// --- UPDATE_PARAMS_UI -------------------------------------------------------

PF_Err SetParamDisabled(AEFX_SuiteScoper<PF_ParamUtilsSuite3> &suite,
                        PF_InData *in_data, PF_ParamIndex position,
                        const PF_ParamDef *current, bool disabled) {
    if (!in_data || !current) return PF_Err_BAD_CALLBACK_PARAM;
    // Copy the host's live definition (never a zeroed struct) and toggle only
    // PF_PUI_DISABLED, matching sub_18000BD10.
    PF_ParamDef def = *current;
    if (disabled) def.ui_flags |= PF_PUI_DISABLED;
    else def.ui_flags &= ~((A_long)PF_PUI_DISABLED);
    return suite->PF_UpdateParamUI(in_data->effect_ref, position, &def);
}

PF_Err SetStreamVisible(AEGP_SuiteHandler &suites, AEGP_EffectRefH effectH,
                        PF_ParamIndex position, bool visible) {
    // sub_18000BED0: AEGP_SetDynamicStreamFlag(HIDDEN, undoable=false, !visible).
    AEGP_StreamRefH streamH = nullptr;
    PF_Err err = suites.StreamSuite7()->AEGP_GetNewEffectStreamByIndex(
        g_aegp_id, effectH, position, &streamH);
    if (!err && streamH) {
        err = suites.DynamicStreamSuite3()->AEGP_SetDynamicStreamFlag(
            streamH, AEGP_DynStreamFlag_HIDDEN, false, visible ? FALSE : TRUE);
        suites.StreamSuite7()->AEGP_DisposeStream(streamH);
    }
    return err;
}

PF_Err UpdateParamsUI(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[]) {
    if (!in_data || !out_data || !params) return PF_Err_BAD_CALLBACK_PARAM;
    if (!in_data->pica_basicP || !g_aegp_id) return PF_Err_NONE;

    const bool per_color = params[OLMCK_PER_COLOR] &&
                           params[OLMCK_PER_COLOR]->u.bd.value != 0;
    const bool per_component = params[OLMCK_PER_COMPONENT] &&
                               params[OLMCK_PER_COMPONENT]->u.bd.value != 0;
    const A_long count = params[OLMCK_COLOR_COUNT] ?
                         params[OLMCK_COLOR_COUNT]->u.sd.value : 0;
    const bool enable_replace = params[OLMCK_ENABLE_REPLACE] &&
                                params[OLMCK_ENABLE_REPLACE]->u.bd.value != 0;

    // Visibility truth table from sub_180001A50:
    //   global Threshold     disabled when perColor || perComponent
    //   global Thr R/G/B     visible only when perComponent && !perColor
    //   slot Color n         visible when n < count; disabled when UseColor n off
    //   slot UseColor n      visible when n < count
    //   slot UseReplace n    visible when n < count && enableReplace;
    //                        disabled when UseColor n off
    //   slot ReplaceColor n  visible when n < count && enableReplace && UseReplace n;
    //                        disabled when UseColor n off
    //   slot Threshold n     visible when n < count && perColor && !perComponent
    //   slot Thr R/G/B n     visible when n < count && perColor && perComponent
    const bool per_color_only = per_color && !per_component;
    const bool both = per_color && per_component;
    AEFX_SuiteScoper<PF_ParamUtilsSuite3> param_utils(
        in_data, kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3, out_data);

    PF_Err err = SetParamDisabled(param_utils, in_data, OLMCK_THRESHOLD,
                                  params[OLMCK_THRESHOLD],
                                  per_color || per_component);
    if (err) return err;

    AEGP_SuiteHandler suites(in_data->pica_basicP);
    AEGP_LayerH layerH = nullptr;
    err = suites.PFInterfaceSuite1()->AEGP_GetEffectLayer(in_data->effect_ref, &layerH);
    if (err || !layerH) return err ? err : PF_Err_BAD_CALLBACK_PARAM;
    AEGP_EffectRefH effectH = nullptr;
    err = suites.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(
        g_aegp_id, in_data->effect_ref, &effectH);
    if (err || !effectH) {
        if (effectH) suites.EffectSuite5()->AEGP_DisposeEffect(effectH);
        return err ? err : PF_Err_BAD_CALLBACK_PARAM;
    }

    const bool component_visible = per_component && !per_color;
    err = SetStreamVisible(suites, effectH, OLMCK_THRESHOLD_R, component_visible);
    if (!err) err = SetStreamVisible(suites, effectH, OLMCK_THRESHOLD_G, component_visible);
    if (!err) err = SetStreamVisible(suites, effectH, OLMCK_THRESHOLD_B, component_visible);

    for (int n = 0; n < OLMCK_MAX_COLORS && !err; ++n) {
        const int base = OLMCK_SLOT_BASE + n * 8;
        const bool active = n < count;
        const bool use_replace = params[base + 1] && params[base + 1]->u.bd.value != 0;
        const bool use_color_off =
            !(params[base + 0] && params[base + 0]->u.bd.value != 0);
        const bool replace_visible = active && enable_replace;
        const bool replace_pickable = replace_visible && use_replace;
        err = SetStreamVisible(suites, effectH, base + 2, active);            // Color n
        if (!err) err = SetStreamVisible(suites, effectH, base + 0, active);  // Use Color n
        if (!err) err = SetStreamVisible(suites, effectH, base + 1, replace_visible);
        if (!err) err = SetStreamVisible(suites, effectH, base + 3, replace_pickable);
        if (!err) err = SetParamDisabled(param_utils, in_data, base + 2,
                                         params[base + 2], use_color_off);
        if (!err) err = SetParamDisabled(param_utils, in_data, base + 1,
                                         params[base + 1], use_color_off);
        if (!err) err = SetParamDisabled(param_utils, in_data, base + 3,
                                         params[base + 3], use_color_off);
        if (!err) err = SetStreamVisible(suites, effectH, base + 4,
                                         active && per_color_only);  // Threshold n
        if (!err) err = SetStreamVisible(suites, effectH, base + 5, active && both);
        if (!err) err = SetStreamVisible(suites, effectH, base + 6, active && both);
        if (!err) err = SetStreamVisible(suites, effectH, base + 7, active && both);
    }
    suites.EffectSuite5()->AEGP_DisposeEffect(effectH);
    return err;
}

// --- PreRender --------------------------------------------------------------

PF_Err PreRender(PF_InData *in_data, PF_PreRenderExtra *extra) {
    PF_CheckoutResult result;
    AEFX_CLR_STRUCT(result);
    PF_RenderRequest request = extra->input->output_request;
    PF_Err err = extra->cb->checkout_layer(in_data->effect_ref, OLMCK_INPUT, 0, &request,
                                           in_data->current_time, in_data->time_step,
                                           in_data->time_scale, &result);
    if (!err) {
        // The binary unions the checked-out rects into the output rects via
        // sub_1800140C0 and sets no output flags and no pre_render_data.
        UnionLRect(&result.result_rect, &extra->output->result_rect);
        UnionLRect(&result.max_result_rect, &extra->output->max_result_rect);
    }
    return err;
}

// --- Color space conversions (binary-exact) ----------------------------------

// 0x180009DA0: RGB -> (h/360, s, v).
void ConvertHSV(float *t) {
    const float r = t[0];
    const float g = t[1];
    const float b = t[2];
    const float bg_max = b <= g ? g : b;
    const float bg_min = g <= b ? g : b;
    const float mx = fmaxf(bg_max, r);
    const float mn = fminf(bg_min, r);
    const float delta = mx - mn;
    float h;
    if (mx == mn) {
        h = 0.0f;
    } else if (r == mx) {
        h = (g - b) * 60.0f / delta;
    } else {
        const float inv = 1.0f / delta;
        if (g == mx) h = (b - r) * 60.0f * inv + 120.0f;
        else h = (r - g) * 60.0f * inv + 240.0f;
    }
    h = fmodf(h, 360.0f);
    if (h < 0.0f) h = h + 360.0f;
    t[0] = h * 0.0027777778f;
    t[1] = mx == 0.0f ? 0.0f : delta / mx;
    t[2] = mx;
}

// 0x180009EE0: RGB (raw, no gamma) -> (L, a, b).
void ConvertLab(float *t) {
    const float r = t[0];
    const float g = t[1];
    const float b = t[2];
    const float ty = (g * 2.1455016f) + (r * 0.63801938f) + (b * 0.21650916f);
    // The binary pre-computes the linear branch of L as a fused transform.
    t[0] = ty <= 0.0088560004f
               ? (g * 1938.0315f) + (r * 576.32294f) + (b * 195.57272f)
               : powf(ty, 0.33333001f) * 116.0f - 16.0f;
    const float tx = (r * 1.2373713f) + (g * 1.0727508f) + (b * 0.54127443f);
    const float fx = tx <= 0.0088560004f
                         ? (tx * 7.7870002f) + 0.13793103f
                         : powf(tx, 0.33333001f);
    const float fy = ty <= 0.0088560004f
                         ? (ty * 7.7870002f) + 0.13793103f
                         : powf(ty, 0.33333001f);
    t[1] = (fx - fy) * 500.0f;
    const float tz = (g * 0.3575826f) + (r * 0.05800258f) + (b * 2.8507097f);
    const float fz = tz <= 0.0088560004f
                         ? (tz * 7.7870002f) + 0.13793103f
                         : powf(tz, 0.33333001f);
    t[2] = (fy - fz) * 200.0f;
}

// Inline YUV in the keyers (0x180001E00/35D0 case 5): RGB -> (Y, U, V).
void ConvertYUV(float *t) {
    const float r = t[0];
    const float g = t[1];
    const float b = t[2];
    t[0] = (g * 0.58700001f) + (r * 0.29899999f) + (b * 0.114f);
    t[2] = (r * 0.61500001f) - (g * 0.51498997f) - (b * 0.10001f);
    t[1] = (b * 0.43599999f) - ((g * 0.28885999f) + (r * 0.14713f));
}

// 0x18000A120: RGB -> (Y, Cb, Cr); only Y and Cb are ever compared.
void ConvertYCrCb(float *t) {
    const float r = t[0];
    const float g = t[1];
    const float b = t[2];
    t[0] = (g * 0.58661002f) + (r * 0.29890999f) + (b * 0.11448f);
    t[2] = (r * 0.5f) - (g * 0.41869f) - (b * 0.081309997f);
    t[1] = (b * 0.5f) - ((r * 0.16874f) + (g * 0.33126f));
}

// --- Distance metrics --------------------------------------------------------
// Signature mirrors the binary: bool fn(key[3], pixel[3], thr[3], weights[3],
// eps, per_component). Lab76 and the Lab94 component path deliberately mutate
// key and pixel in place; the keyer preserves that behaviour (the per-pixel
// pixel triple accumulates the +133.037/+163.48801 shifts across enabled
// slots because the caller never resets it, exactly like the binary).

bool DistRGB(float *key, float *pixel, float *thr, float *w, float eps, char per_comp) {
    if (per_comp) {
        return fabsf(pixel[0] - key[0]) <= (thr[0] * w[0]) + eps &&
               fabsf(pixel[1] - key[1]) <= (thr[1] * w[1]) + eps &&
               fabsf(pixel[2] - key[2]) <= (thr[2] * w[2]) + eps;
    }
    const float d1 = key[1] - pixel[1];
    const float d2 = key[2] - pixel[2];
    const float limit = sqrtf((w[1] * w[1]) + (w[0] * w[0]) + (w[2] * w[2])) *
                        (eps + thr[0]);
    const float dist = sqrtf(((key[0] - pixel[0]) * (key[0] - pixel[0])) + (d1 * d1) +
                             (d2 * d2));
    return limit >= dist;
}

bool DistHSV(float *key, float *pixel, float *thr, float *w, float eps, char per_comp) {
    if (per_comp) {
        float hue = pixel[0];
        if (pixel[0] < key[0]) hue = hue + 1.0f;  // one-sided circular hue only here
        return (hue - key[0]) <= (thr[0] * w[0]) + eps &&
               fabsf(pixel[1] - key[1]) <= (thr[1] * w[1]) + eps &&
               fabsf(pixel[2] - key[2]) <= (thr[2] * w[2]) + eps;
    }
    const float d1 = key[1] - pixel[1];
    const float d2 = key[2] - pixel[2];
    const float limit = sqrtf((w[1] * w[1]) + (w[0] * w[0]) + (w[2] * w[2])) *
                        (eps + thr[0]);
    const float dist = sqrtf(((key[0] - pixel[0]) * (key[0] - pixel[0])) + (d1 * d1) +
                             (d2 * d2));
    return limit >= dist;
}

bool DistLab76(float *key, float *pixel, float *thr, float *w, float eps, char per_comp) {
    // Always shifts BOTH arguments in place (binary 0x1800043A0).
    const float pb = pixel[2] + kLab76ShiftB;
    pixel[1] = pixel[1] + kLab76ShiftA;
    pixel[2] = pb;
    const float kb = key[2] + kLab76ShiftB;
    key[1] = key[1] + kLab76ShiftA;
    key[2] = kb;
    w[0] = 151.30099f;
    w[1] = 264.367f;
    w[2] = 295.573f;
    if (per_comp) {
        return fabsf(pixel[0] - key[0]) <= (eps * 2709.9299f) + (thr[0] * 151.30099f) &&
               fabsf(pixel[1] - key[1]) <= (thr[1] * 264.367f) + (eps * 578.71399f) &&
               fabsf(pixel[2] - key[2]) <= (thr[2] * 295.573f) + (eps * 414.67599f);
    }
    const float d1 = key[1] - pixel[1];
    const float limit = (eps * 424.43527f) + (thr[0] * 424.43527f);
    const float dist = sqrtf(((key[0] - pixel[0]) * (key[0] - pixel[0])) + (d1 * d1) +
                             ((key[2] - pixel[2]) * (key[2] - pixel[2])));
    return limit >= dist;
}

bool DistLab94(float *key, float *pixel, float *thr, float *w, float eps, char per_comp) {
    if (per_comp) {
        // Component path shifts both arguments in place (same quirk as Lab76).
        const float pb = pixel[2] + kLab76ShiftB;
        pixel[1] = pixel[1] + kLab76ShiftA;
        pixel[2] = pb;
        const float ka = key[1] + kLab76ShiftA;
        key[2] = key[2] + kLab76ShiftB;
        key[1] = ka;
        w[0] = 151.30099f;
        w[1] = 264.367f;
        w[2] = 295.573f;
        return fabsf(pixel[0] - key[0]) <= (eps + thr[0]) * 151.30099f &&
               fabsf(pixel[1] - key[1]) <= (eps + thr[1]) * 264.367f &&
               fabsf(pixel[2] - key[2]) <= (eps + thr[2]) * 295.573f;
    }
    // Aggregate path uses the UNSHIFTED a/b (verified 0x180004510 else-branch).
    const float limit = (eps + thr[0]) * 352.978f;
    const float dl = pixel[0] - key[0];
    const float c1 = sqrtf((key[2] * key[2]) + (key[1] * key[1]));
    const float c2 = sqrtf((pixel[2] * pixel[2]) + (pixel[1] * pixel[1]));
    const float dc = c2 - c1;
    const float cbar = sqrtf(c2 * c1);
    const float h1_raw = atan2f(key[2], key[1]) * kDegPerRad;
    float h1 = h1_raw + 180.0f;
    const float h2_raw = atan2f(pixel[2], pixel[1]) * kDegPerRad;
    float h2 = h2_raw + 180.0f;
    if (h1_raw + 180.0f != 0.0f) {
        if (h1 < 0.0f) h1 = h1_raw + 540.0f;
        h1 = fmodf(h1, 360.0f);
    }
    if (h2 != 0.0f) {
        if (h2 < 0.0f) h2 = h2_raw + 540.0f;
        h2 = fmodf(h2, 360.0f);
    }
    const float dh = h2 - h1;
    const float chroma_scale = (cbar * 0.045000002f) + 1.0f;
    const float hue_scale = (cbar * 0.015f) + 1.0f;
    const float dcq = dc / chroma_scale;
    const float dhq = dh / hue_scale;
    // v12 = pixel.L - key.L: the first term is the LIGHTNESS delta squared.
    const float term = (dl * dl) + (dcq * dcq) + (dhq * dhq);
    return limit >= sqrtf(term);
}

bool DistYUV(float *key, float *pixel, float *thr, float *w, float eps, char per_comp) {
    // Third condition ignores V entirely; the U term maps through double
    // math (0x180004850: cvtps2pd/mulsd/addsd/cvtpd2ps).
    const float thr_b = per_comp ? thr[1] : thr[0];
    const float thr_c = per_comp ? thr[2] : thr[0];
    const float limit_b = (thr_b * w[1]) + eps;
    const float limit_c = (thr_c * w[2]) + eps;
    return fabsf(pixel[0] - key[0]) <= (thr[0] * w[0]) + eps &&
           fabsf((float)((double)pixel[1] * 1.146788990825688 + 0.5) -
                 (float)((double)key[1] * 1.146788990825688 + 0.5)) <= limit_b &&
           limit_c >= 0.0f;
}

bool DistYCrCb(float *key, float *pixel, float *thr, float *w, float eps, char per_comp) {
    const float thr_b = per_comp ? thr[1] : thr[0];
    const float thr_c = per_comp ? thr[2] : thr[0];
    const float limit_b = (thr_b * w[1]) + eps;
    const float limit_c = (thr_c * w[2]) + eps;
    return fabsf(pixel[0] - key[0]) <= (thr[0] * w[0]) + eps &&
           fabsf(pixel[1] - key[1]) <= limit_b &&
           limit_c >= 0.0f;
}

typedef bool (*DistFn)(float *, float *, float *, float *, float, char);

// --- Per-pixel keyer ---------------------------------------------------------

template <typename Pixel>
struct PixelCodec;

template <>
struct PixelCodec<PF_Pixel> {
    static constexpr float kDecode = 0.0039215689f;  // binary 8-bit constant
    static constexpr float kWriteMult = 255.0f;
    // v arrives already scaled by kWriteMult (binary: (int)(float)(mult * v)).
    static A_u_char WriteChannel(float v) { return (A_u_char)(int)v; }
    static A_u_char ScaleAlpha(float v) { return (A_u_char)(int)v; }
};

template <>
struct PixelCodec<PF_Pixel16> {
    static constexpr float kDecode = 0.000030517578f;  // 1/32768
    static constexpr float kWriteMult = 32768.0f;
    static A_u_short WriteChannel(float v) { return (A_u_short)(int)v; }
    static A_u_short ScaleAlpha(float v) { return (A_u_short)(int)v; }
};

template <>
struct PixelCodec<PF_PixelFloat> {
    static constexpr float kDecode = 1.0f;
    static constexpr float kWriteMult = 1.0f;
    static float WriteChannel(float v) { return v; }
    static float ScaleAlpha(float v) { return v; }
};

struct KeyerContext {
    const OLMCKParams *params;
};

template <typename Pixel>
PF_Err KeyerPixel(void *refcon, A_long, A_long, Pixel *in, Pixel *out) {
    const OLMCKParams &p = *static_cast<const KeyerContext *>(refcon)->params;
    float src[4];
    src[0] = (float)in->red * PixelCodec<Pixel>::kDecode;
    src[1] = (float)in->green * PixelCodec<Pixel>::kDecode;
    src[2] = (float)in->blue * PixelCodec<Pixel>::kDecode;
    src[3] = (float)in->alpha * PixelCodec<Pixel>::kDecode;

    const int count = (int)p.count;
    float keys[OLMCK_MAX_COLORS][3];
    for (int i = 0; i < count; ++i) {
        keys[i][0] = p.key_colors[i].red;
        keys[i][1] = p.key_colors[i].green;
        keys[i][2] = p.key_colors[i].blue;
    }

    if (p.premultiplied) {
        src[0] *= src[3];
        src[1] *= src[3];
        src[2] *= src[3];
    }

    DistFn dist = DistRGB;
    switch (p.color_space) {
        case OLMCK_SPACE_HSV:
            dist = DistHSV;
            ConvertHSV(src);
            for (int i = 0; i < count; ++i) ConvertHSV(keys[i]);
            break;
        case OLMCK_SPACE_LAB76:
            dist = DistLab76;
            ConvertLab(src);
            for (int i = 0; i < count; ++i) ConvertLab(keys[i]);
            break;
        case OLMCK_SPACE_LAB94:
            dist = DistLab94;
            ConvertLab(src);
            for (int i = 0; i < count; ++i) ConvertLab(keys[i]);
            break;
        case OLMCK_SPACE_YUV:
            dist = DistYUV;
            ConvertYUV(src);
            for (int i = 0; i < count; ++i) ConvertYUV(keys[i]);
            break;
        case OLMCK_SPACE_YCRCB:
            dist = DistYCrCb;
            ConvertYCrCb(src);
            for (int i = 0; i < count; ++i) ConvertYCrCb(keys[i]);
            break;
        default:
            break;  // RGB: raw triples
    }

    // Threshold triple selection (0x180001E00 threshold block):
    //   both modes          -> per-slot (R,G,B) triple, per-component compare
    //   perColor only       -> per-slot scalar replicated, aggregate compare
    //   perComponent only   -> global (R,G,B) triple, per-component compare
    //   neither             -> global scalar replicated (0 when global == 0)
    float thr[3];
    char per_comp_arg = 0;
    if (p.per_color && p.per_component) {
        per_comp_arg = 1;
    } else if (!p.per_color && p.per_component) {
        per_comp_arg = 1;
    }

    int hit = -1;
    bool matched = false;
    if (p.per_color) {
        for (int i = 0; i < count; ++i) {
            if (!p.use_color[i]) continue;
            if (p.per_component) {
                thr[0] = p.threshold_rgbs[i][0];
                thr[1] = p.threshold_rgbs[i][1];
                thr[2] = p.threshold_rgbs[i][2];
            } else {
                thr[0] = p.thresholds[i];
                thr[1] = p.thresholds[i];
                thr[2] = p.thresholds[i];
            }
            float weights[3] = {1.0f, 1.0f, 1.0f};
            if (dist(keys[i], src, thr, weights, p.epsilon, per_comp_arg)) {
                hit = i;
                matched = true;
                break;
            }
        }
    } else {
        if (p.per_component) {
            thr[0] = p.threshold_rgb[0];
            thr[1] = p.threshold_rgb[1];
            thr[2] = p.threshold_rgb[2];
        } else {
            thr[0] = p.global_threshold;
            thr[1] = p.global_threshold;
            thr[2] = p.global_threshold;
        }
        for (int i = 0; i < count; ++i) {
            if (!p.use_color[i]) continue;
            float weights[3] = {1.0f, 1.0f, 1.0f};
            if (dist(keys[i], src, thr, weights, p.epsilon, per_comp_arg)) {
                hit = i;
                matched = true;
                break;
            }
        }
    }

    if (p.keep && hit >= 0 && p.use_replace[hit] && p.enable_replace) {
        const float mult = PixelCodec<Pixel>::kWriteMult;
        out->red = PixelCodec<Pixel>::WriteChannel(mult * p.replace_colors[hit].red);
        out->green = PixelCodec<Pixel>::WriteChannel(mult * p.replace_colors[hit].green);
        out->blue = PixelCodec<Pixel>::WriteChannel(mult * p.replace_colors[hit].blue);
    } else {
        out->red = in->red;
        out->green = in->green;
        out->blue = in->blue;
    }
    out->alpha = matched ? in->alpha : PixelCodec<Pixel>::WriteChannel(0.0f);
    return PF_Err_NONE;
}

template <typename Pixel>
PF_Err InvertPixel(void *, A_long, A_long, Pixel *in, Pixel *out) {
    // Color Keep OFF: alpha = source alpha - keyed alpha (sub_1800029B0 and
    // its 16-bit/float twins). RGB is left untouched.
    out->alpha = PixelCodec<Pixel>::ScaleAlpha((float)in->alpha - (float)out->alpha);
    return PF_Err_NONE;
}

// --- Iterate helper ----------------------------------------------------------

template <typename Suite, typename Pixel>
PF_Err RunIterate(PF_InData *in_data, PF_OutData *out_data, const char *suite_name,
                  int suite_version, PF_EffectWorld *src, PF_EffectWorld *dst,
                  const PF_Rect *area,
                  PF_Err (*pix_fn)(void *, A_long, A_long, Pixel *, Pixel *),
                  void *refcon) {
    AEFX_SuiteScoper<Suite> iterate_suite(in_data, suite_name, suite_version, out_data);
    A_long progress_final;
    if (area) {
        progress_final = area->bottom - area->top + 1;
        if (progress_final < 0) progress_final = 0;
    } else {
        progress_final = dst->height;
    }
    return iterate_suite->iterate(in_data, 0, progress_final, src, area, refcon, pix_fn, dst);
}

PF_Err KeyerIterate(PF_InData *in_data, PF_OutData *out_data, PF_EffectWorld *src,
                    PF_EffectWorld *dst, const PF_Rect *area, const OLMCKParams &p) {
    KeyerContext context{&p};
    switch (p.bitdepth) {
        case 8:
            return RunIterate<PF_Iterate8Suite1, PF_Pixel>(
                in_data, out_data, kPFIterate8Suite, kPFIterate8SuiteVersion1, src, dst,
                area, KeyerPixel<PF_Pixel>, &context);
        case 16:
            return RunIterate<PF_Iterate16Suite1, PF_Pixel16>(
                in_data, out_data, kPFIterate16Suite, kPFIterate16SuiteVersion1, src, dst,
                area, KeyerPixel<PF_Pixel16>, &context);
        default:
            return RunIterate<PF_IterateFloatSuite1, PF_PixelFloat>(
                in_data, out_data, kPFIterateFloatSuite, kPFIterateFloatSuiteVersion1, src,
                dst, area, KeyerPixel<PF_PixelFloat>, &context);
    }
}

PF_Err InvertIterate(PF_InData *in_data, PF_OutData *out_data, PF_EffectWorld *src,
                     PF_EffectWorld *dst, const PF_Rect *area, A_long bitdepth) {
    switch (bitdepth) {
        case 8:
            return RunIterate<PF_Iterate8Suite1, PF_Pixel>(
                in_data, out_data, kPFIterate8Suite, kPFIterate8SuiteVersion1, src, dst,
                area, InvertPixel<PF_Pixel>, nullptr);
        case 16:
            return RunIterate<PF_Iterate16Suite1, PF_Pixel16>(
                in_data, out_data, kPFIterate16Suite, kPFIterate16SuiteVersion1, src, dst,
                area, InvertPixel<PF_Pixel16>, nullptr);
        default:
            return RunIterate<PF_IterateFloatSuite1, PF_PixelFloat>(
                in_data, out_data, kPFIterateFloatSuite, kPFIterateFloatSuiteVersion1, src,
                dst, area, InvertPixel<PF_PixelFloat>, nullptr);
    }
}

// --- Edge pipeline -----------------------------------------------------------

template <typename Pixel>
Pixel *TypedPixel(PF_EffectWorld &world, A_long x, A_long y) {
    return (Pixel *)((char *)world.data + (size_t)y * (size_t)world.rowbytes +
                     (size_t)x * sizeof(Pixel));
}

float *FloatPixel(PF_EffectWorld &world, A_long x, A_long y) {
    return (float *)((char *)world.data + (size_t)y * (size_t)world.rowbytes +
                     (size_t)x * sizeof(PF_PixelFloat));
}

void SetFloatAll(float *p, float v) {
    p[0] = v;
    p[1] = v;
    p[2] = v;
    p[3] = v;
}

// Band mask (0x180008A60/8C20/8DD0): init opaque; a keyed pixel with an
// in-bounds 8-neighbour of zero alpha becomes the seed ring. Self excluded,
// out-of-bounds neighbours ignored, so the ring is the shape's inner boundary.
template <typename Pixel>
void BuildBandMask(PF_EffectWorld &src, PF_EffectWorld &mask, A_long width, A_long height) {
    using C = PixelCodec<Pixel>;
    for (A_long y = 0; y < height; ++y) {
        for (A_long x = 0; x < width; ++x) {
            Pixel *mp = TypedPixel<Pixel>(mask, x, y);
            mp->alpha = C::WriteChannel(C::kWriteMult);
            mp->green = C::WriteChannel(C::kWriteMult);
            if (TypedPixel<Pixel>(src, x, y)->alpha == 0) continue;
            bool boundary = false;
            for (int dy = -1; dy <= 1 && !boundary; ++dy) {
                for (int dx = -1; dx <= 1 && !boundary; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const A_long nx = x + dx;
                    const A_long ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                    if (TypedPixel<Pixel>(src, nx, ny)->alpha == 0) boundary = true;
                }
            }
            if (boundary) mp->alpha = C::WriteChannel(0.0f);
        }
    }
}

// Felzenszwalb/Huttenlocher 1D squared-distance transform, exactly as
// 0x18000A710 (including the +/-1.00000002e20 envelope sentinels and the
// num / (2w(k-l)) * (1/w) intersection arithmetic).
void Edt1D(float *out_f, const float *in_g, A_long *last, float *z, int n, float w) {
    if (n <= 0) return;
    const int n1 = n - 1;
    float neg_inf_like;
    float pos_inf_like;
    const uint32_t neg_bits = 0xE0AD78ECu;  // -1.00000002e20f
    const uint32_t pos_bits = 0x60AD78ECu;  // +1.00000002e20f
    std::memcpy(&neg_inf_like, &neg_bits, 4);
    std::memcpy(&pos_inf_like, &pos_bits, 4);
    last[0] = 0;
    z[0] = neg_inf_like;
    z[1] = pos_inf_like;
    int s = 0;
    if (n1 >= 1) {
        const float inv_w = 1.0f / w;
        int k = 1;
        do {
            float q = 0.0f;
            for (;;) {
                const int ls = last[s];
                const float num = ((float)((float)(k * k) * w) * w + in_g[k]) -
                                  (((w * (float)ls) * (w * (float)ls)) + in_g[ls]);
                const float den = (2.0f * (float)k * w) - (2.0f * (float)ls * w);
                q = num / den * inv_w;
                if (!(q <= z[s])) break;
                --s;
            }
            ++s;
            last[s] = k;
            z[s] = q;
            z[s + 1] = pos_inf_like;
            ++k;
        } while (k <= n1);
    }
    int s2 = 0;
    for (int i = 0; i <= n1; ++i) {
        while ((float)i > z[s2 + 1]) ++s2;
        const int ls = last[s2];
        const int d = i - ls;
        out_f[i] = ((float)((float)(d * d) * w) * w) + in_g[ls];
    }
}

// 0x18000A920: separable squared EDT — columns scaled by wy, rows by wx.
void Edt2D(float *f, int w, int h, int wx, int wy) {
    if (h <= 0 || w <= 0) return;
    const int span = w > h ? w : h;
    std::vector<float> g((size_t)span);
    std::vector<float> line((size_t)span);
    std::vector<A_long> last((size_t)span + 1);
    std::vector<float> z((size_t)span + 2);
    for (int x = 0; x < w; ++x) {
        for (int k = 0; k < h; ++k) g[(size_t)k] = f[(size_t)k * (size_t)w + (size_t)x];
        Edt1D(line.data(), g.data(), last.data(), z.data(), h, (float)wy);
        for (int k = 0; k < h; ++k) f[(size_t)k * (size_t)w + (size_t)x] = line[(size_t)k];
    }
    for (int y = 0; y < h; ++y) {
        float *row = f + (size_t)y * (size_t)w;
        Edt1D(line.data(), row, last.data(), z.data(), w, (float)wx);
        for (int k = 0; k < w; ++k) row[k] = line[k];
    }
}

// Euclidean distance (0x180007CA0/7ED0/8100): keyed pixel -> 1e20, boundary
// ring -> 0; FH transform; sqrt per pixel; every channel of C gets the value.
// No 4000 clamp here (unlike the chamfers) — no-site distances stay enormous.
template <typename Pixel>
void EuclideanDistance(PF_InData *in_data, PF_EffectWorld &mask, PF_EffectWorld &dist,
                       A_long width, A_long height) {
    const A_long wx = in_data->downsample_x.den;
    const A_long wy = in_data->downsample_y.den;
    std::vector<float> grid((size_t)width * (size_t)height);
    for (A_long y = 0; y < height; ++y) {
        for (A_long x = 0; x < width; ++x) {
            grid[(size_t)y * (size_t)width + (size_t)x] =
                TypedPixel<Pixel>(mask, x, y)->alpha != 0 ? 1.0e20f : 0.0f;
        }
    }
    Edt2D(grid.data(), (int)width, (int)height, (int)wx, (int)wy);
    for (A_long y = 0; y < height; ++y) {
        for (A_long x = 0; x < width; ++x) {
            SetFloatAll(FloatPixel(dist, x, y),
                        sqrtf(grid[(size_t)y * (size_t)width + (size_t)x]));
        }
    }
}

// Box chamfer (0x1800066F0 family): forward (left, up-left, up, up-right) and
// mirrored backward (right, down-right, down, down-left) passes; clamp 4000;
// distance 0 exactly on the mask ring; seed corners get clamp-1.
// Degenerate 1-pixel-wide/high canvases get the plain 1D distance (axis step
// per pixel, 0 on the mask ring, cap 4000, big start consistent with the
// no-seed saturation) — the multi-column backward quirks below do not apply.
template <typename Pixel>
void BoxChamfer(PF_EffectWorld &mask, PF_EffectWorld &dist, A_long width, A_long height,
                float wx, float wy, float clamp_value) {
    const float big = clamp_value - 1.0f;
    if (width == 1 || height == 1) {
        const A_long n = width == 1 ? height : width;
        const float step = width == 1 ? wy : wx;
        std::vector<float> d((size_t)n);
        // Convention as in the 2D passes: mask != 0 propagates (big start),
        // mask == 0 is a seed with distance exactly 0. The forward pass
        // accumulates from the previous pixel whether or not a seed has been
        // seen (cap applies), so no-seed rows saturate at 4000 away from the
        // two scan starts, matching the 2D corner behaviour.
        bool seeded = false;
        for (A_long i = 0; i < n; ++i) {
            const bool on = width == 1
                                ? TypedPixel<Pixel>(mask, 0, i)->alpha != 0
                                : TypedPixel<Pixel>(mask, i, 0)->alpha != 0;
            if (!on) {
                d[(size_t)i] = 0.0f;
                seeded = true;
            } else {
                d[(size_t)i] = i > 0 ? std::min(d[(size_t)(i - 1)] + step, clamp_value)
                                     : big;
            }
        }
        // backward
        seeded = false;
        for (A_long i = n - 1; i >= 0; --i) {
            const bool on = width == 1
                                ? TypedPixel<Pixel>(mask, 0, i)->alpha != 0
                                : TypedPixel<Pixel>(mask, i, 0)->alpha != 0;
            if (!on) {
                seeded = true;
            } else if (seeded) {
                d[(size_t)i] = std::min(d[(size_t)i],
                                        std::min(d[(size_t)(i + 1)] + step, clamp_value));
            }
        }
        for (A_long i = 0; i < n; ++i) {
            if (width == 1) SetFloatAll(FloatPixel(dist, 0, i), d[(size_t)i]);
            else SetFloatAll(FloatPixel(dist, i, 0), d[(size_t)i]);
        }
        return;
    }
    for (A_long x = 0; x < width; ++x) {
        float v = TypedPixel<Pixel>(mask, x, 0)->alpha != 0 ? big : 0.0f;
        if (x > 0 && v != 0.0f) {
            const float left = *FloatPixel(dist, x - 1, 0);
            v = std::min(left + wx, clamp_value);
        }
        SetFloatAll(FloatPixel(dist, x, 0), v);
    }
    for (A_long y = 1; y < height; ++y) {
        float carried = 0.0f;
        {
            const bool on = TypedPixel<Pixel>(mask, 0, y)->alpha != 0;
            if (on && width > 1) {
                const float up = *FloatPixel(dist, 0, y - 1);
                const float up_right = *FloatPixel(dist, 1, y - 1);
                carried = std::min(std::min(up_right, up) + wy, clamp_value);
            } else if (on) {
                carried = big;
            }
            SetFloatAll(FloatPixel(dist, 0, y), carried);
        }
        for (A_long x = 1; x + 1 < width; ++x) {
            const bool on = TypedPixel<Pixel>(mask, x, y)->alpha != 0;
            if (on) {
                const float cand_left = carried + wx;
                const float up = *FloatPixel(dist, x, y - 1);
                const float up_left = *FloatPixel(dist, x - 1, y - 1);
                const float up_right = *FloatPixel(dist, x + 1, y - 1);
                const float m = std::min(std::min(up_right, up_left), up) + wy;
                carried = std::min(cand_left, m);
                if (clamp_value < carried) carried = clamp_value;
            } else {
                carried = 0.0f;
            }
            SetFloatAll(FloatPixel(dist, x, y), carried);
        }
        if (width > 1) {
            const A_long x = width - 1;
            const bool on = TypedPixel<Pixel>(mask, x, y)->alpha != 0;
            float v = 0.0f;
            if (on) {
                const float cand_left = carried + wx;
                const float up = *FloatPixel(dist, x, y - 1);
                const float up_left = *FloatPixel(dist, x - 1, y - 1);
                const float m = std::min(up, up_left) + wy;
                v = std::min(cand_left, m);
                if (clamp_value < v) v = clamp_value;
            }
            SetFloatAll(FloatPixel(dist, x, y), v);
        }
    }
    // Backward: bottom row propagates leftwards, then rows upward.
    for (A_long x = width - 2; x >= 0; --x) {
        const float right = *FloatPixel(dist, x + 1, height - 1);
        const float cand = std::min(right + wx, clamp_value);
        float *p = FloatPixel(dist, x, height - 1);
        SetFloatAll(p, std::min(p[0], cand));
    }
    for (A_long y = height - 2; y >= 0; --y) {
        A_long x = width - 1;
        {
            float v = big;
            if (width > 1) {
                const float down = *FloatPixel(dist, x, y + 1);
                const float down_left = *FloatPixel(dist, x - 1, y + 1);
                v = std::min(down, down_left) + wy;
                if (clamp_value < v) v = clamp_value;
            }
            float *p = FloatPixel(dist, x, y);
            SetFloatAll(p, std::min(p[0], v));
            --x;
        }
        for (; x > 0; --x) {
            const float down = *FloatPixel(dist, x, y + 1);
            const float down_left = *FloatPixel(dist, x - 1, y + 1);
            const float down_right = *FloatPixel(dist, x + 1, y + 1);
            const float right = *FloatPixel(dist, x + 1, y);
            const float m = std::min(std::min(down_right, down_left),
                                     std::min(down, right)) + wy;
            float v = std::min(m, right + wx);
            if (clamp_value < v) v = clamp_value;
            float *p = FloatPixel(dist, x, y);
            SetFloatAll(p, std::min(p[0], v));
        }
        {
            const float down = *FloatPixel(dist, 0, y + 1);
            const float down_right = width > 1 ? *FloatPixel(dist, 1, y + 1) : down;
            const float right = width > 1 ? *FloatPixel(dist, 1, y) : 0.0f;
            const float m = std::min(std::min(down_right, down), right) + wy;
            float v = std::min(m, right + wx);
            if (clamp_value < v) v = clamp_value;
            float *p = FloatPixel(dist, 0, y);
            SetFloatAll(p, std::min(p[0], v));
        }
    }
}

// Approximate chamfer (0x1800058A0 family): only left/up and right/down moves;
// the backward pass mirrors the binary exactly, including its
// min(min(down, right) + wy, right + wx) candidate pair.
template <typename Pixel>
void ApproxChamfer(PF_EffectWorld &mask, PF_EffectWorld &dist, A_long width, A_long height,
                   float wx, float wy, float clamp_value) {
    const float big = clamp_value - 1.0f;
    for (A_long x = 0; x < width; ++x) {
        float v = TypedPixel<Pixel>(mask, x, 0)->alpha != 0 ? big : 0.0f;
        if (x > 0 && v != 0.0f) {
            const float left = *FloatPixel(dist, x - 1, 0);
            v = std::min(left + wx, clamp_value);
        }
        SetFloatAll(FloatPixel(dist, x, 0), v);
    }
    for (A_long y = 1; y < height; ++y) {
        float carried = 0.0f;
        for (A_long x = 0; x < width; ++x) {
            const bool on = TypedPixel<Pixel>(mask, x, y)->alpha != 0;
            if (on) {
                const float up = *FloatPixel(dist, x, y - 1);
                float v = x > 0 ? std::min(up + wy, carried + wx) : std::min(up + wy, clamp_value);
                if (clamp_value < v) v = clamp_value;
                carried = v;
            } else {
                carried = 0.0f;
            }
            SetFloatAll(FloatPixel(dist, x, y), carried);
        }
    }
    for (A_long x = width - 2; x >= 0; --x) {
        const float right = *FloatPixel(dist, x + 1, height - 1);
        const float cand = std::min(right + wx, clamp_value);
        float *p = FloatPixel(dist, x, height - 1);
        SetFloatAll(p, std::min(p[0], cand));
    }
    for (A_long y = height - 2; y >= 0; --y) {
        A_long x = width - 1;
        {
            float v = big;
            if (height > 1) {
                const float down = *FloatPixel(dist, x, y + 1);
                v = std::min(down + wy, clamp_value);
            }
            float *p = FloatPixel(dist, x, y);
            SetFloatAll(p, std::min(p[0], v));
            --x;
        }
        for (; x >= 0; --x) {
            const float down = *FloatPixel(dist, x, y + 1);
            const float right = *FloatPixel(dist, x + 1, y);
            float v = std::min(std::min(down, right) + wy, right + wx);
            if (clamp_value < v) v = clamp_value;
            float *p = FloatPixel(dist, x, y);
            SetFloatAll(p, std::min(p[0], v));
        }
    }
}

// Blur weight profiles (0x1800049A0/4B50/4CF0 and the word/float twins).
template <typename Pixel>
void ProfileInside(float amount, PF_EffectWorld &keyed, PF_EffectWorld &dist,
                   A_long width, A_long height, float *weights) {
    const float step = kPiF / amount;
    for (A_long y = 0; y < height; ++y) {
        float *row = weights + (size_t)y * (size_t)width;
        for (A_long x = 0; x < width; ++x) {
            const float alpha = (float)TypedPixel<Pixel>(keyed, x, y)->alpha;
            const float d = *FloatPixel(dist, x, y);
            float w;
            if (alpha != 0) {
                w = d >= amount ? 1.0f : (sinf(d * step - kHalfPi) + 1.0f) * 0.5f;
            } else {
                w = 0.0f;
            }
            row[x] = w;
        }
    }
}

template <typename Pixel>
void ProfileAround(float amount, PF_EffectWorld &keyed, PF_EffectWorld &dist,
                   A_long width, A_long height, float *weights) {
    const float step = kAroundStep / amount;
    for (A_long y = 0; y < height; ++y) {
        float *row = weights + (size_t)y * (size_t)width;
        for (A_long x = 0; x < width; ++x) {
            const float alpha = (float)TypedPixel<Pixel>(keyed, x, y)->alpha;
            const float d = *FloatPixel(dist, x, y);
            float w;
            if (d == 0.0f) {
                w = 0.5f;
            } else if (d >= amount) {
                w = alpha != 0 ? 1.0f : 0.0f;
            } else {
                float a = d * step;
                if (alpha == 0) a = -a;
                w = (sinf(a) + 1.0f) * 0.5f;
            }
            row[x] = w;
        }
    }
}

template <typename Pixel>
void ProfileOutside(float amount, PF_EffectWorld &keyed, PF_EffectWorld &dist,
                    A_long width, A_long height, float *weights) {
    const float step = kPiF / amount;
    for (A_long y = 0; y < height; ++y) {
        float *row = weights + (size_t)y * (size_t)width;
        for (A_long x = 0; x < width; ++x) {
            const float alpha = (float)TypedPixel<Pixel>(keyed, x, y)->alpha;
            const float d = *FloatPixel(dist, x, y);
            float w;
            if (alpha != 0) {
                w = 1.0f;
            } else if (d >= amount) {
                w = 0.0f;
            } else {
                w = (sinf(kHalfPi - d * step) + 1.0f) * 0.5f;
            }
            row[x] = w;
        }
    }
}

// Blur apply (0x1800085A0/8340/8800): writes ONLY alpha into the output (the
// keyed RGB got there via the canvas copy), truncating integer encode for
// 8/16 bpc, raw float for 32 bpc. A transparent keyed pixel with non-zero
// weight takes the source alpha.
template <typename Pixel>
void ApplyBlur(PF_EffectWorld &keyed, PF_EffectWorld &dist, PF_EffectWorld &input,
               PF_EffectWorld &output, A_long width, A_long height, A_long direction,
               float amount) {
    std::vector<float> weights((size_t)width * (size_t)height);
    if (direction == OLMCK_DIR_INSIDE) {
        ProfileInside<Pixel>(amount, keyed, dist, width, height, weights.data());
    } else if (direction == OLMCK_DIR_AROUND) {
        ProfileAround<Pixel>(amount, keyed, dist, width, height, weights.data());
    } else {
        ProfileOutside<Pixel>(amount, keyed, dist, width, height, weights.data());
    }
    for (A_long y = 0; y < height; ++y) {
        const float *row = weights.data() + (size_t)y * (size_t)width;
        for (A_long x = 0; x < width; ++x) {
            const float w = row[x];
            const float base = (float)TypedPixel<Pixel>(keyed, x, y)->alpha;
            float value;
            if (base == 0.0f && w != 0.0f) {
                value = w * (float)TypedPixel<Pixel>(input, x, y)->alpha;
            } else {
                value = base * w;
            }
            TypedPixel<Pixel>(output, x, y)->alpha =
                PixelCodec<Pixel>::ScaleAlpha(value);
        }
    }
}

// Edge distance dispatch shared by the thin and blur stages.
template <typename Pixel>
void BuildDistance(PF_InData *in_data, PF_EffectWorld &mask, PF_EffectWorld &dist,
                   A_long distance_type, A_long width, A_long height) {
    const float wx = (float)in_data->downsample_x.den;
    const float wy = (float)in_data->downsample_y.den;
    switch (distance_type) {
        case OLMCK_DIST_BOX:
            BoxChamfer<Pixel>(mask, dist, width, height, wx, wy, kClampDistance);
            break;
        case OLMCK_DIST_APPROX:
            ApproxChamfer<Pixel>(mask, dist, width, height, wx, wy, kClampDistance);
            break;
        case OLMCK_DIST_EUCLIDEAN:
            EuclideanDistance<Pixel>(in_data, mask, dist, width, height);
            break;
        default:
            break;  // binary leaves the cleared distance canvas in place
    }
}

// Thin pass (inside the 8/16/32 kernels): thin <= 0 erodes keyed alpha,
// thin > 0 restores the FULL source pixel (RGB and alpha) under the mask.
// The binary round-trips restored pixels through float; verified bit-exact
// against that round trip for all 256/32768 code values, so a direct copy
// is used here.
template <typename Pixel>
void ApplyThin(PF_EffectWorld &keyed, PF_EffectWorld &dist, PF_EffectWorld &input,
               A_long width, A_long height, A_long thin_amount) {
    const float thin = (float)thin_amount;
    for (A_long y = 0; y < height; ++y) {
        for (A_long x = 0; x < width; ++x) {
            Pixel *kp = TypedPixel<Pixel>(keyed, x, y);
            const float d = *FloatPixel(dist, x, y);
            if (thin <= 0.0f) {
                if (kp->alpha != 0 && -thin > d) kp->alpha = PixelCodec<Pixel>::WriteChannel(0.0f);
            } else {
                if (kp->alpha == 0 && thin >= d) {
                    *kp = *TypedPixel<Pixel>(input, x, y);
                }
            }
        }
    }
}

// --- World RAII --------------------------------------------------------------

class ScopedWorld {
public:
    ScopedWorld() : suiteP(nullptr), effect_ref(nullptr), created(false) {
        AEFX_CLR_STRUCT(world);
    }
    ~ScopedWorld() {
        if (created && suiteP) suiteP->PF_DisposeWorld(effect_ref, &world);
    }
    PF_Err Create(PF_InData *in_data, const PF_WorldSuite2 *suite, A_long width,
                  A_long height, PF_PixelFormat format) {
        suiteP = suite;
        effect_ref = in_data->effect_ref;
        const PF_Err err = suite->PF_NewWorld(effect_ref, width, height, TRUE, format,
                                              &world);
        created = err == PF_Err_NONE;
        return err;
    }
    PF_EffectWorld *get() { return &world; }

private:
    ScopedWorld(const ScopedWorld &);
    ScopedWorld &operator=(const ScopedWorld &);
    PF_EffectWorld world;
    const PF_WorldSuite2 *suiteP;
    PF_ProgPtr effect_ref;
    bool created;
};

PF_PixelFormat FormatForDepth(A_long bitdepth) {
    return bitdepth == 8 ? PF_PixelFormat_ARGB32
                         : bitdepth == 16 ? PF_PixelFormat_ARGB64
                                          : PF_PixelFormat_ARGB128;
}

// --- Kernels -----------------------------------------------------------------

template <typename Pixel>
PF_Err Kernel(PF_InData *in_data, PF_OutData *out_data, PF_EffectWorld *input,
              PF_EffectWorld *output, const OLMCKParams &p) {
    if (!in_data->utils || !in_data->utils->copy) return PF_Err_BAD_CALLBACK_PARAM;
    PF_Err err = in_data->utils->copy(in_data->effect_ref, input, output, nullptr, nullptr);
    if (err) return err;

    const PF_Rect *area = &output->extent_hint;

    // No thin and no blur: the binary still keys every pixel (and inverts for
    // Color Keep OFF) directly in the output via the iterate suite.
    if (p.thin_amount == 0 && p.blur_amount == 0.0f) {
        err = KeyerIterate(in_data, out_data, input, output, area, p);
        if (!err && !p.keep)
            err = InvertIterate(in_data, out_data, input, output, area, p.bitdepth);
        return err;
    }

    const A_long width = input->width;
    const A_long height = input->height;
    AEFX_SuiteScoper<PF_WorldSuite2> world_suite(in_data, kPFWorldSuite,
                                                 kPFWorldSuiteVersion2, out_data);
    const PF_PixelFormat format = FormatForDepth(p.bitdepth);
    ScopedWorld canvas_a;
    err = canvas_a.Create(in_data, world_suite.get(), width, height, format);
    if (err) return err;
    ScopedWorld canvas_b;
    err = canvas_b.Create(in_data, world_suite.get(), width, height, format);
    if (err) return err;
    ScopedWorld canvas_c;
    err = canvas_c.Create(in_data, world_suite.get(), width, height, PF_PixelFormat_ARGB128);
    if (err) return err;

    err = KeyerIterate(in_data, out_data, input, canvas_a.get(), area, p);
    if (err) return err;

    if (p.thin_amount != 0) {
        BuildBandMask<Pixel>(*canvas_a.get(), *canvas_b.get(), width, height);
        BuildDistance<Pixel>(in_data, *canvas_b.get(), *canvas_c.get(), p.thin_distance,
                             width, height);
        ApplyThin<Pixel>(*canvas_a.get(), *canvas_c.get(), *input, width, height,
                         p.thin_amount);
    }

    err = in_data->utils->copy(in_data->effect_ref, canvas_a.get(), output, nullptr,
                               nullptr);
    if (err) return err;

    if (p.blur_amount != 0.0f) {
        BuildBandMask<Pixel>(*canvas_a.get(), *canvas_b.get(), width, height);
        BuildDistance<Pixel>(in_data, *canvas_b.get(), *canvas_c.get(), p.blur_distance,
                             width, height);
        ApplyBlur<Pixel>(*canvas_a.get(), *canvas_c.get(), *input, *output, width, height,
                         p.blur_direction, p.blur_amount);
    }

    if (!p.keep) err = InvertIterate(in_data, out_data, input, output, area, p.bitdepth);
    return err;
}

PF_Err SmartRender(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra) {
    PF_EffectWorld *input = nullptr;
    PF_EffectWorld *output = nullptr;
    const PF_Err input_err = extra->cb->checkout_layer_pixels(in_data->effect_ref,
                                                              OLMCK_INPUT, &input);
    const PF_Err output_err = extra->cb->checkout_output(in_data->effect_ref, &output);
    PF_Err err = input_err ? input_err : output_err;
    const bool checked_out = input_err == PF_Err_NONE;
    if (!err && (!input || !output)) err = PF_Err_BAD_CALLBACK_PARAM;

    try {
        OLMCKParams params;
        if (!err) err = CheckoutParams(in_data, out_data, extra->input->bitdepth, &params);
        if (!err) {
            switch (extra->input->bitdepth) {
                case 8:
                    err = Kernel<PF_Pixel>(in_data, out_data, input, output, params);
                    break;
                case 16:
                    err = Kernel<PF_Pixel16>(in_data, out_data, input, output, params);
                    break;
                case 32:
                    err = Kernel<PF_PixelFloat>(in_data, out_data, input, output, params);
                    break;
                default:
                    err = PF_Err_BAD_CALLBACK_PARAM;
                    break;
            }
        }
    } catch (...) {
        // Suite-acquisition and allocation failures must not skip the layer
        // checkin; rethrow so EffectMain keeps translating the original error.
        if (checked_out)
            extra->cb->checkin_layer_pixels(in_data->effect_ref, OLMCK_INPUT);
        throw;
    }
    // The original never calls checkin_layer_pixels (the host reclaims at
    // frame end); the port checks in to keep checkout accounting balanced.
    if (checked_out) {
        const PF_Err checkin_err = extra->cb->checkin_layer_pixels(in_data->effect_ref,
                                                                   OLMCK_INPUT);
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
            case PF_Cmd_UPDATE_PARAMS_UI:
                return UpdateParamsUI(in_data, out_data, params);
            case PF_Cmd_SMART_PRE_RENDER:
                return PreRender(in_data, (PF_PreRenderExtra *)extra);
            case PF_Cmd_SMART_RENDER:
                return SmartRender(in_data, out_data, (PF_SmartRenderExtra *)extra);
            case PF_Cmd_RENDER:
            default:
                // The binary no-ops every other command (USER_CHANGED_PARAM
                // included; SUPERVISE lives only on Number of Colors and
                // Enable Replace there).
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

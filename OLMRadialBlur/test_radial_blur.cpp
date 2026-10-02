// Fake only the AE host callbacks; production EffectMain, checkout, pre-render,
// kernels, polar passes, and pixel writers are exercised below.
#include "OLMRadialBlur.cpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr A_long kWidth = 33;
constexpr A_long kHeight = 33;
constexpr float kCenter = 16.5f;

struct TestHost {
    PF_PointParamSuite1 point_suite{};
    PF_ParamUtilsSuite3 param_utils_suite{};
    SPBasicSuite basic{};
    PF_UtilCallbacks utils{};
    PF_InData in{};
    PF_OutData out{};
    PF_EffectWorld input{};
    PF_EffectWorld output{};
    PF_PreRenderInput pre_input{};
    PF_PreRenderOutput pre_output{};
    PF_PreRenderCallbacks pre_callbacks{};
    PF_PreRenderExtra pre_extra{};
    PF_SmartRenderInput render_input{};
    PF_SmartRenderCallbacks render_callbacks{};
    PF_SmartRenderExtra render_extra{};
    std::array<PF_ParamDef, OLMRB_NUM_PARAMS> params{};
    std::array<unsigned, OLMRB_NUM_PARAMS> checkouts{};
    std::array<unsigned, OLMRB_NUM_PARAMS> checkins{};
    std::array<PF_ParamDef, OLMRB_NUM_PARAMS> added{};
    size_t add_count = 0;
    unsigned layer_checkouts = 0;
    unsigned layer_checkins = 0;
    unsigned output_checkouts = 0;
    unsigned pre_render_checkouts = 0;
    PF_ParamIndex expected_ui_position = PF_ParamIndex_NONE;
    PF_ParamDef expected_ui_def{};
    PF_ParamUIFlags expected_ui_flags = PF_PUI_NONE;
    unsigned ui_callback_count = 0;
    bool ui_callback_valid = true;
    float point_x = kCenter;
    float point_y = kCenter;
    int bitdepth = 8;
    std::vector<unsigned char> input_bytes;
    std::vector<unsigned char> output_bytes;
};

TestHost *g_host = nullptr;

bool same_ui_target_fields(const PF_ParamDef &actual, const PF_ParamDef &expected) {
    if (actual.uu.id != expected.uu.id || actual.ui_width != expected.ui_width ||
        actual.ui_height != expected.ui_height || actual.param_type != expected.param_type ||
        std::strcmp(actual.PF_DEF_NAME, expected.PF_DEF_NAME) ||
        actual.flags != expected.flags || actual.unused != expected.unused)
        return false;

    switch (expected.param_type) {
    case PF_Param_SLIDER:
        return actual.u.sd.value == expected.u.sd.value &&
               !std::strcmp(actual.u.sd.value_str, expected.u.sd.value_str) &&
               !std::strcmp(actual.u.sd.value_desc, expected.u.sd.value_desc) &&
               actual.u.sd.valid_min == expected.u.sd.valid_min &&
               actual.u.sd.valid_max == expected.u.sd.valid_max &&
               actual.u.sd.slider_min == expected.u.sd.slider_min &&
               actual.u.sd.slider_max == expected.u.sd.slider_max &&
               actual.u.sd.dephault == expected.u.sd.dephault;
    case PF_Param_POPUP:
        return actual.u.pd.value == expected.u.pd.value &&
               actual.u.pd.num_choices == expected.u.pd.num_choices &&
               actual.u.pd.dephault == expected.u.pd.dephault &&
               actual.u.pd.u.PF_DEF_NAMESPTR == expected.u.pd.u.PF_DEF_NAMESPTR;
    default:
        return false;
    }
}

int position_for_id(A_long id) {
    if (!g_host) return -1;
    for (int i = 1; i < OLMRB_NUM_PARAMS; ++i)
        if (g_host->added[(size_t)i].uu.id == id) return i;
    return -1;
}

PF_Err add_param(PF_ProgPtr, PF_ParamIndex, PF_ParamDefPtr def) {
    if (!g_host || g_host->add_count >= OLMRB_NUM_PARAMS - 1)
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    ++g_host->add_count;
    g_host->added[g_host->add_count] = *def;
    g_host->params[g_host->add_count] = *def;
    return PF_Err_NONE;
}

PF_Err checkout_param(PF_ProgPtr, PF_ParamIndex position, A_long, A_long, A_u_long,
                      PF_ParamDef *param) {
    if (!g_host || position <= OLMRB_INPUT || position >= OLMRB_NUM_PARAMS || !param)
        return PF_Err_BAD_CALLBACK_PARAM;
    ++g_host->checkouts[(size_t)position];
    *param = g_host->params[(size_t)position];
    return PF_Err_NONE;
}

PF_Err checkin_param(PF_ProgPtr, PF_ParamDef *param) {
    if (!g_host || !param) return PF_Err_BAD_CALLBACK_PARAM;
    const int position = position_for_id(param->uu.id);
    if (position < 1) return PF_Err_BAD_CALLBACK_PARAM;
    ++g_host->checkins[(size_t)position];
    return PF_Err_NONE;
}

PF_Err checkin_layer_pixels(PF_ProgPtr, A_long position) {
    if (!g_host || position != OLMRB_INPUT) return PF_Err_BAD_CALLBACK_PARAM;
    ++g_host->layer_checkins;
    return PF_Err_NONE;
}

PF_Err checkout_layer_pixels(PF_ProgPtr, A_long position, PF_EffectWorld **world) {
    if (!g_host || !world || position != OLMRB_INPUT) return PF_Err_BAD_CALLBACK_PARAM;
    ++g_host->layer_checkouts;
    *world = &g_host->input;
    return PF_Err_NONE;
}

PF_Err checkout_output(PF_ProgPtr, PF_EffectWorld **world) {
    if (!g_host || !world) return PF_Err_BAD_CALLBACK_PARAM;
    ++g_host->output_checkouts;
    *world = &g_host->output;
    return PF_Err_NONE;
}

PF_Err pre_render_checkout(PF_ProgPtr, PF_ParamIndex position, A_long checkout_id,
                           const PF_RenderRequest *, A_long, A_long, A_u_long,
                           PF_CheckoutResult *result) {
    if (!g_host || !result || position != OLMRB_INPUT || checkout_id != 0)
        return PF_Err_BAD_CALLBACK_PARAM;
    ++g_host->pre_render_checkouts;
    result->result_rect = g_host->input.extent_hint;
    result->max_result_rect = g_host->input.extent_hint;
    result->ref_width = g_host->in.width;
    result->ref_height = g_host->in.height;
    return PF_Err_NONE;
}

PF_Err copy_world(PF_ProgPtr, PF_EffectWorld *src, PF_EffectWorld *dst,
                  PF_Rect *, PF_Rect *) {
    if (!src || !dst || !src->data || !dst->data || src->rowbytes != dst->rowbytes ||
        src->height != dst->height)
        return PF_Err_BAD_CALLBACK_PARAM;
    std::memcpy(dst->data, src->data, (size_t)src->rowbytes * (size_t)src->height);
    return PF_Err_NONE;
}

PF_Err get_point(PF_ProgPtr, const PF_ParamDef *, A_FloatPoint *point) {
    if (!g_host || !point) return PF_Err_BAD_CALLBACK_PARAM;
    point->x = g_host->point_x;
    point->y = g_host->point_y;
    return PF_Err_NONE;
}

PF_Err SPAPI update_param_ui(PF_ProgPtr effect_ref, PF_ParamIndex position,
                             const PF_ParamDef *def) {
    if (!g_host || effect_ref != g_host->in.effect_ref || !def ||
        position != g_host->expected_ui_position)
        return PF_Err_BAD_CALLBACK_PARAM;
    ++g_host->ui_callback_count;
    if (def->ui_flags != g_host->expected_ui_flags ||
        !same_ui_target_fields(*def, g_host->expected_ui_def)) {
        g_host->ui_callback_valid = false;
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    return PF_Err_NONE;
}

PF_Err SPAPI acquire_suite(const char *name, int32 version, const void **suite) {
    if (!g_host || !name || !suite) return (SPErr)-1;
    if (!std::strcmp(name, kPFPointParamSuite) && version == kPFPointParamSuiteVersion1)
        *suite = &g_host->point_suite;
    else if (!std::strcmp(name, kPFParamUtilsSuite) &&
             version == kPFParamUtilsSuiteVersion3)
        *suite = &g_host->param_utils_suite;
    else
        return (SPErr)-1;
    return kSPNoError;
}

SPErr SPAPI release_suite(const char *, int32) { return kSPNoError; }

template <typename Pixel, typename Channel>
void fill_pixels(std::vector<unsigned char> &bytes, A_long rowbytes, float divisor) {
    for (int y = 0; y < kHeight; ++y) {
        Pixel *row = (Pixel *)(bytes.data() + (size_t)y * (size_t)rowbytes);
        for (int x = 0; x < kWidth; ++x) {
            const float dx = (float)x - kCenter;
            const float dy = (float)y - kCenter;
            float alpha = 0.0f;
            if (dx * dx + dy * dy <= 49.0f) {
                const int pattern = (x * 3 + y * 5) % 9;
                alpha = 0.18f + 0.82f * (float)pattern / 8.0f;
            }
            const float red = alpha * (0.12f + 0.72f * (float)x / (float)kWidth);
            const float green = alpha * (0.14f + 0.68f * (float)y / (float)kHeight);
            const float blue = alpha * (0.72f - 0.42f * (float)x / (float)kWidth);
            row[x].alpha = (Channel)std::lround(alpha * divisor);
            row[x].red = (Channel)std::lround(red * divisor);
            row[x].green = (Channel)std::lround(green * divisor);
            row[x].blue = (Channel)std::lround(blue * divisor);
        }
    }
}

void fill_float_pixels(std::vector<unsigned char> &bytes, A_long rowbytes) {
    for (int y = 0; y < kHeight; ++y) {
        PF_PixelFloat *row = (PF_PixelFloat *)(bytes.data() + (size_t)y * (size_t)rowbytes);
        for (int x = 0; x < kWidth; ++x) {
            const float dx = (float)x - kCenter;
            const float dy = (float)y - kCenter;
            float alpha = 0.0f;
            if (dx * dx + dy * dy <= 49.0f) {
                const int pattern = (x * 3 + y * 5) % 9;
                alpha = 0.18f + 0.82f * (float)pattern / 8.0f;
            }
            row[x].alpha = alpha;
            row[x].red = alpha * (0.12f + 0.72f * (float)x / (float)kWidth);
            row[x].green = alpha * (0.14f + 0.68f * (float)y / (float)kHeight);
            row[x].blue = alpha * (0.72f - 0.42f * (float)x / (float)kWidth);
        }
    }
}

void set_world(PF_EffectWorld &world, std::vector<unsigned char> &bytes,
               A_long rowbytes) {
    world.data = (PF_PixelPtr)bytes.data();
    world.rowbytes = rowbytes;
    world.width = kWidth;
    world.height = kHeight;
    world.origin_x = 0;
    world.origin_y = 0;
    world.extent_hint.left = 0;
    world.extent_hint.top = 0;
    world.extent_hint.right = kWidth;
    world.extent_hint.bottom = kHeight;
}

bool initialize_host(TestHost &host, int bitdepth) {
    g_host = &host;
    host.bitdepth = bitdepth;
    host.point_suite.PF_GetFloatingPointValueFromPointDef = get_point;
    host.param_utils_suite.PF_UpdateParamUI = update_param_ui;
    host.basic.AcquireSuite = acquire_suite;
    host.basic.ReleaseSuite = release_suite;
    host.utils.copy = copy_world;

    const A_long pixel_size = bitdepth == 8 ? sizeof(PF_Pixel)
                            : bitdepth == 16 ? sizeof(PF_Pixel16)
                                             : sizeof(PF_PixelFloat);
    const A_long rowbytes = kWidth * pixel_size;
    host.input_bytes.assign((size_t)rowbytes * (size_t)kHeight, 0);
    host.output_bytes.assign((size_t)rowbytes * (size_t)kHeight, 0xcd);
    if (bitdepth == 8) fill_pixels<PF_Pixel, A_u_char>(host.input_bytes, rowbytes, 255.0f);
    else if (bitdepth == 16) fill_pixels<PF_Pixel16, A_u_short>(host.input_bytes, rowbytes, 32768.0f);
    else fill_float_pixels(host.input_bytes, rowbytes);
    set_world(host.input, host.input_bytes, rowbytes);
    set_world(host.output, host.output_bytes, rowbytes);

    host.in.inter.add_param = add_param;
    host.in.inter.checkout_param = checkout_param;
    host.in.inter.checkin_param = checkin_param;
    host.in.utils = &host.utils;
    host.in.effect_ref = (PF_ProgPtr)&host;
    host.in.pica_basicP = &host.basic;
    host.in.current_time = 0;
    host.in.time_step = 1;
    host.in.time_scale = 30;
    host.in.width = kWidth;
    host.in.height = kHeight;
    host.in.downsample_x.num = 1;
    host.in.downsample_x.den = 1;
    host.in.downsample_y.num = 1;
    host.in.downsample_y.den = 1;

    if (EffectMain(PF_Cmd_PARAMS_SETUP, &host.in, &host.out, nullptr, nullptr, nullptr) !=
            PF_Err_NONE ||
        host.out.num_params != OLMRB_NUM_PARAMS || host.add_count != OLMRB_NUM_PARAMS - 1)
        return false;

    host.pre_input.output_request.rect.left = 5;
    host.pre_input.output_request.rect.top = 4;
    host.pre_input.output_request.rect.right = 27;
    host.pre_input.output_request.rect.bottom = 29;
    host.pre_input.bitdepth = (short)bitdepth;
    host.pre_output.result_rect = host.pre_input.output_request.rect;
    host.pre_output.max_result_rect = host.pre_input.output_request.rect;
    host.pre_callbacks.checkout_layer = pre_render_checkout;
    host.pre_extra.input = &host.pre_input;
    host.pre_extra.output = &host.pre_output;
    host.pre_extra.cb = &host.pre_callbacks;

    host.render_callbacks.checkout_layer_pixels = checkout_layer_pixels;
    host.render_callbacks.checkin_layer_pixels = checkin_layer_pixels;
    host.render_callbacks.checkout_output = checkout_output;
    host.render_input.bitdepth = (short)bitdepth;
    host.render_input.output_request = host.pre_input.output_request;
    host.render_extra.input = &host.render_input;
    host.render_extra.cb = &host.render_callbacks;
    return true;
}

bool verify_params(const TestHost &host) {
    if (host.add_count != 30 || host.out.num_params != 31) return false;
    const PF_ParamDef &blur = host.added[OLMRB_BLUR_TYPE];
    const PF_ParamDef &center = host.added[OLMRB_CENTER];
    const PF_ParamDef &outer_group = host.added[OLMRB_OUTER_GROUP];
    const PF_ParamDef &strength = host.added[OLMRB_OUTER_STRENGTH];
    const PF_ParamDef &offset_mode = host.added[OLMRB_OUTER_OFFSET_MODE];
    const PF_ParamDef &outer_offset = host.added[OLMRB_OUTER_OFFSET];
    const PF_ParamDef &outer_fade = host.added[OLMRB_OUTER_FADE];
    const PF_ParamDef &inner_strength = host.added[OLMRB_INNER_STRENGTH];
    const PF_ParamDef &repeat = host.added[OLMRB_REPEAT_BORDER];
    const PF_ParamDef &ratio = host.added[OLMRB_RATIO];
    const PF_ParamDef &quality = host.added[OLMRB_QUALITY];
    const PF_ParamDef &brightness = host.added[OLMRB_BRIGHTNESS];
    const PF_ParamDef &size_variation = host.added[OLMRB_SIZE_VARIATION];
    const PF_ParamDef &noise_variation = host.added[OLMRB_NOISE_VARIATION];
    const PF_ParamDef &noise_type = host.added[OLMRB_NOISE_TYPE];
    const PF_ParamDef &noise_layer = host.added[OLMRB_NOISE_LAYER];
    const PF_ParamDef &seed = host.added[OLMRB_SEED];
    const PF_ParamDef &thickness = host.added[OLMRB_THICKNESS];
    const PF_ParamDef &last = host.added[OLMRB_NOISE_GROUP_END];
    return blur.uu.id == OLMRB_ID_BLUR_TYPE && blur.param_type == PF_Param_POPUP &&
           blur.u.pd.num_choices == 2 && blur.u.pd.dephault == OLMRB_BLUR_ZOOM &&
           (blur.flags & PF_ParamFlag_SUPERVISE) &&
           center.uu.id == OLMRB_ID_CENTER && center.param_type == PF_Param_POINT &&
           center.u.td.restrict_bounds == FALSE && FIX_2_FLOAT(center.u.td.x_dephault) == 50.0 &&
           FIX_2_FLOAT(center.u.td.y_dephault) == 50.0 &&
           outer_group.param_type == PF_Param_GROUP_START &&
           (outer_group.flags & PF_ParamFlag_COLLAPSE_TWIRLY) &&
           strength.uu.id == OLMRB_ID_OUTER_STRENGTH && strength.param_type == PF_Param_SLIDER &&
           strength.u.sd.valid_min == 0 && strength.u.sd.valid_max == 2000 &&
           strength.u.sd.dephault == 0 && offset_mode.uu.id == OLMRB_ID_OUTER_OFFSET_MODE &&
           offset_mode.param_type == PF_Param_POPUP && offset_mode.u.pd.num_choices == 3 &&
           (offset_mode.flags & PF_ParamFlag_SUPERVISE) && outer_offset.u.sd.dephault == 0 &&
           outer_fade.u.sd.dephault == 0 && inner_strength.u.sd.dephault == 0 &&
           repeat.uu.id == OLMRB_ID_REPEAT_BORDER && repeat.param_type == PF_Param_CHECKBOX &&
           repeat.u.bd.dephault && ratio.param_type == PF_Param_FLOAT_SLIDER &&
           ratio.u.fs_d.valid_min == 1.0 && ratio.u.fs_d.valid_max == 5.0 &&
           ratio.u.fs_d.dephault == 1.0 && quality.param_type == PF_Param_FLOAT_SLIDER &&
           quality.u.fs_d.dephault == 5.0 && brightness.u.fs_d.dephault == 1.0 &&
           size_variation.u.fs_d.dephault == 0.0 && noise_variation.u.fs_d.dephault == 0.0 &&
           noise_type.param_type == PF_Param_POPUP &&
           noise_type.u.pd.num_choices == 3 && seed.param_type == PF_Param_SLIDER &&
           noise_layer.param_type == PF_Param_LAYER && seed.u.sd.valid_min == 1 &&
           seed.u.sd.valid_max == 1000 && seed.u.sd.dephault == 1 &&
           thickness.u.fs_d.dephault == 10.0 &&
           last.param_type == PF_Param_GROUP_END;
}

bool verify_param_ui_updates() {
    TestHost host;
    if (!initialize_host(host, 8) || !verify_params(host)) {
        std::fprintf(stderr, "UPDATE_PARAMS_UI test host setup failed\n");
        g_host = nullptr;
        return false;
    }

    const PF_ParamIndex positions[] = {
        OLMRB_OUTER_STRENGTH, OLMRB_OUTER_OFFSET_MODE, OLMRB_OUTER_OFFSET,
        OLMRB_INNER_STRENGTH, OLMRB_INNER_OFFSET_MODE, OLMRB_INNER_OFFSET
    };
    std::array<PF_ParamDef *, OLMRB_NUM_PARAMS> ui_params{};
    for (int i = 0; i < OLMRB_NUM_PARAMS; ++i) ui_params[(size_t)i] = &host.params[(size_t)i];
    for (const PF_ParamIndex position : positions)
        host.params[position].ui_flags = PF_PUI_ECW_SEPARATOR;

    if (EffectMain(PF_Cmd_UPDATE_PARAMS_UI, &host.in, &host.out, nullptr,
                   nullptr, nullptr) != PF_Err_BAD_CALLBACK_PARAM) {
        std::fprintf(stderr, "UPDATE_PARAMS_UI accepted a null params array\n");
        g_host = nullptr;
        return false;
    }
    ui_params[OLMRB_INNER_OFFSET_MODE] = nullptr;
    if (EffectMain(PF_Cmd_UPDATE_PARAMS_UI, &host.in, &host.out, ui_params.data(),
                   nullptr, nullptr) != PF_Err_BAD_CALLBACK_PARAM) {
        std::fprintf(stderr, "UPDATE_PARAMS_UI accepted a null target definition\n");
        g_host = nullptr;
        return false;
    }
    ui_params[OLMRB_INNER_OFFSET_MODE] = &host.params[OLMRB_INNER_OFFSET_MODE];

    AEFX_SuiteScoper<PF_ParamUtilsSuite3> suite(
        &host.in, kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3, &host.out);
    for (const PF_ParamIndex position : positions) {
        host.expected_ui_position = position;
        const PF_ParamUIFlags host_flags = host.params[position].ui_flags;
        for (PF_Boolean disabled : {TRUE, FALSE}) {
            PF_ParamDef current = host.params[position];
            if (!disabled) current.ui_flags |= PF_PUI_DISABLED;
            host.expected_ui_def = current;
            const PF_ParamUIFlags source_flags = current.ui_flags;
            host.expected_ui_flags = source_flags;
            if (disabled) host.expected_ui_flags |= PF_PUI_DISABLED;
            else host.expected_ui_flags &= ~PF_PUI_DISABLED;
            host.ui_callback_count = 0;
            host.ui_callback_valid = true;

            const PF_Err err = SetParamDisabled(
                suite, &host.in, position, &current, disabled);
            if (err || !host.ui_callback_valid || host.ui_callback_count != 1 ||
                !same_ui_target_fields(current, host.expected_ui_def) ||
                current.ui_flags != source_flags ||
                !same_ui_target_fields(host.params[position], host.expected_ui_def) ||
                host.params[position].ui_flags != host_flags) {
                std::fprintf(stderr,
                             "UPDATE_PARAMS_UI changed or corrupted param %d (%s)\n",
                             (int)position, disabled ? "disabled" : "enabled");
                g_host = nullptr;
                return false;
            }
        }
    }

    g_host = nullptr;
    std::puts("UPDATE_PARAMS_UI: six controls enabled/disabled with definitions preserved");
    return true;
}

struct RenderResult {
    std::vector<unsigned char> input;
    std::vector<unsigned char> output;
};

bool render(int bitdepth, int mode, bool repeat_border, RenderResult *result) {
    TestHost host;
    if (!initialize_host(host, bitdepth)) return false;
    if (!verify_params(host)) return false;

    host.params[OLMRB_REPEAT_BORDER].u.bd.value = repeat_border ? 1 : 0;
    if (mode == 1) {
        host.params[OLMRB_OUTER_STRENGTH].u.sd.value = 8;
        host.params[OLMRB_OUTER_FADE].u.sd.value = 8;
    } else if (mode == 2) {
        host.params[OLMRB_BLUR_TYPE].u.pd.value = OLMRB_BLUR_ROTATION;
        host.params[OLMRB_INNER_STRENGTH].u.sd.value = 8;
        host.params[OLMRB_INNER_OFFSET_MODE].u.pd.value = OLMRB_OFFSET_ADD;
        host.params[OLMRB_INNER_OFFSET].u.sd.value = 5;
    }

    const PF_Err pre_err = EffectMain(PF_Cmd_SMART_PRE_RENDER, &host.in, &host.out,
                                      nullptr, nullptr, &host.pre_extra);
    PF_LRect expected = host.pre_input.output_request.rect;
    UnionLRect(&host.input.extent_hint, &expected);
    if (pre_err != PF_Err_NONE || host.pre_render_checkouts != 1 ||
        std::memcmp(&host.pre_output.result_rect, &expected, sizeof(expected)) != 0 ||
        !(host.pre_output.flags & PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS))
        return false;

    const PF_Err render_err = EffectMain(PF_Cmd_SMART_RENDER, &host.in, &host.out,
                                         nullptr, nullptr, &host.render_extra);
    bool params_balanced = true;
    for (int i = 1; i < OLMRB_NUM_PARAMS; ++i)
        params_balanced &= host.checkouts[(size_t)i] == host.checkins[(size_t)i];
    const bool callbacks_balanced = host.layer_checkouts == 1 && host.layer_checkins == 1 &&
                                    host.output_checkouts == 1;
    result->input = std::move(host.input_bytes);
    result->output = std::move(host.output_bytes);
    g_host = nullptr;
    return render_err == PF_Err_NONE && params_balanced && callbacks_balanced;
}

size_t changed_pixels(const RenderResult &a, const RenderResult &b, int bitdepth,
                      bool *outside_shape = nullptr) {
    const size_t pixel_size = bitdepth == 8 ? sizeof(PF_Pixel)
                            : bitdepth == 16 ? sizeof(PF_Pixel16)
                                             : sizeof(PF_PixelFloat);
    size_t changed = 0;
    if (outside_shape) *outside_shape = false;
    for (size_t p = 0; p < a.output.size() / pixel_size; ++p) {
        const size_t start = p * pixel_size;
        if (std::memcmp(a.output.data() + start, b.output.data() + start, pixel_size) == 0)
            continue;
        ++changed;
        if (outside_shape) {
            const int x = (int)(p % (size_t)kWidth);
            const int y = (int)(p / (size_t)kWidth);
            const float dx = (float)x - kCenter;
            const float dy = (float)y - kCenter;
            if (dx * dx + dy * dy > 18.0f * 18.0f) *outside_shape = true;
        }
    }
    return changed;
}

bool run_all() {
    if (!verify_param_ui_updates()) return false;

    for (int bitdepth : {8, 16, 32}) {
        RenderResult baseline, zoom;
        if (!render(bitdepth, 0, true, &baseline) || !render(bitdepth, 1, true, &zoom)) {
            std::fprintf(stderr, "%d-bit setup/render callbacks failed\n", bitdepth);
            return false;
        }
        if (baseline.output != baseline.input) {
            std::fprintf(stderr, "%d-bit default pass-through differs from input\n", bitdepth);
            return false;
        }
        bool outside_shape = false;
        const size_t zoom_changes = changed_pixels(baseline, zoom, bitdepth, &outside_shape);
        if (!zoom_changes || outside_shape) {
            std::fprintf(stderr, "%d-bit zoom/fade delta missing or escaped alpha neighborhood\n",
                         bitdepth);
            return false;
        }
        std::printf("%d-bit zoom + edge fade: %zu changed pixels; outside neighborhood: no\n",
                    bitdepth, zoom_changes);

        RenderResult rotation;
        if (!render(bitdepth, 2, true, &rotation)) {
            std::fprintf(stderr, "%d-bit rotation render failed\n", bitdepth);
            return false;
        }
        const size_t rotation_changes = changed_pixels(baseline, rotation, bitdepth);
        if (!rotation_changes) {
            std::fprintf(stderr, "%d-bit rotation + offset did not change output\n", bitdepth);
            return false;
        }
        std::printf("%d-bit rotation + inner offset: %zu changed pixels\n",
                    bitdepth, rotation_changes);
    }

    RenderResult repeat_on, repeat_off, baseline;
    if (!render(8, 0, true, &baseline) || !render(8, 1, true, &repeat_on) ||
        !render(8, 1, false, &repeat_off) ||
        !changed_pixels(baseline, repeat_on, 8) || !changed_pixels(baseline, repeat_off, 8)) {
        std::fprintf(stderr, "Repeat Border on/off did not render both paths\n");
        return false;
    }
    std::puts("Repeat Border on/off: both render paths passed");
    std::puts("PASS: 31 parameter positions, pass-through, Zoom/Rotation, and 8/16/32-bpc SmartRender.");
    return true;
}

} // namespace

int main() {
    return run_all() ? 0 : 1;
}

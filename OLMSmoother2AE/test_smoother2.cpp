// Fake AE host; production EffectMain runs PARAMS_SETUP -> SMART_PRE_RENDER ->
// SMART_RENDER below. Covers: 16-param identities/defaults, UPDATE_PARAMS_UI
// visibility for all control modes via mocked AEGP suites (full-def pass
// through, zeroed-def rejection), v1/v2 passthrough at 8/16/32 bpc, color
// key + invert on RGB only, edge distance/edge-byte layout, the run-gated
// corner blend (0.4/0.2/0.4 split at 0.5 base, wsum 0.5) checked against an
// independently computed weight sum, the isolated-diagonal corner mix
// (0.4/0.2/0.4 at k = 0.125), the v2 gamma All-Colors pow path on a real
// blend, and checkout/checkin balance.
#include "OLMSmoother2.cpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {

constexpr A_long kWidth = 9;
constexpr A_long kHeight = 7;

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond, msg) \
    do { \
        ++g_checks; \
        if (!(cond)) { \
            std::printf("FAIL: %s\n", msg); \
            ++g_failures; \
            return false; \
        } \
    } while (0)

struct WorldBuffer {
    std::vector<unsigned char> bytes;
    A_long rowbytes;
};

struct TestHost {
    int bitdepth = 8;
    A_long width = kWidth;
    A_long height = kHeight;
    SPBasicSuite basic{};
    PF_UtilCallbacks utils{};
    PF_InData in{};
    PF_OutData out{};
    std::vector<unsigned char> input_bytes;
    std::vector<unsigned char> output_bytes;
    PF_EffectWorld input{};
    PF_EffectWorld output{};
    A_long rowbytes = 0;
    A_long pixel_size = 4;

    std::array<PF_ParamDef, OLMS2_NUM_PARAMS> params{};
    std::array<PF_ParamDef, OLMS2_NUM_PARAMS> added{};
    int add_count = 0;
    int param_checkouts = 0;
    int param_checkins = 0;
    int layer_checkouts = 0;
    int layer_checkins = 0;
    int output_checkouts = 0;

    PF_ColorParamSuite1 color_suite{};
    PF_ParamUtilsSuite3 param_utils_suite{};
    PF_WorldSuite2 world_suite{};
    AEGP_UtilitySuite3 util_suite{};
    AEGP_PFInterfaceSuite1 pf_interface{};
    AEGP_EffectSuite5 effect_suite{};
    AEGP_StreamSuite7 stream_suite{};
    AEGP_DynamicStreamSuite3 dyn_stream{};

    struct UiRecord {
        PF_ParamIndex position;
        PF_ParamDef def;
    };
    std::vector<UiRecord> ui_records;
    struct StreamRecord {
        PF_ParamIndex position;
        A_Boolean hidden;  // true == stream hidden
    };
    std::vector<StreamRecord> stream_records;
    int effect_disposes = 0;
    int stream_disposes = 0;

    std::vector<WorldBuffer *> world_buffers;
    int color_suite_calls = 0;
    AEGP_PluginID registered_id = 0;
    bool ui_reject_zeroed = false;

    A_long PixelSize() const { return bitdepth == 8 ? 4 : bitdepth == 16 ? 8 : 16; }
};

TestHost *g_host = nullptr;

template <typename Pixel>
Pixel *WorldPixel(PF_EffectWorld &world, A_long x, A_long y) {
    return (Pixel *)((char *)world.data + (size_t)y * (size_t)world.rowbytes +
                     (size_t)x * sizeof(Pixel));
}

A_u_long Encode(TestHost &host, float value) {
    if (host.bitdepth == 8) return (A_u_long)(A_u_char)(int)((value * 255.0f) + 0.5f);
    if (host.bitdepth == 16) return (A_u_long)(A_u_short)(int)((value * 32768.0f) + 0.5f);
    A_u_long bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}

float Decode(TestHost &host, A_u_long raw) {
    if (host.bitdepth == 8) return (float)(A_u_char)raw * 0.0039215689f;
    if (host.bitdepth == 16) return (float)(A_u_short)raw * 0.000030517578f;
    float v;
    std::memcpy(&v, &raw, 4);
    return v;
}

struct PixelValues {
    float a, r, g, b;
};

PixelValues ReadPixel(TestHost &host, PF_EffectWorld &world, A_long x, A_long y) {
    PixelValues v{0, 0, 0, 0};
    if (host.bitdepth == 8) {
        const PF_Pixel *p = WorldPixel<PF_Pixel>(world, x, y);
        v.a = Decode(host, p->alpha);
        v.r = Decode(host, p->red);
        v.g = Decode(host, p->green);
        v.b = Decode(host, p->blue);
    } else if (host.bitdepth == 16) {
        const PF_Pixel16 *p = WorldPixel<PF_Pixel16>(world, x, y);
        v.a = Decode(host, p->alpha);
        v.r = Decode(host, p->red);
        v.g = Decode(host, p->green);
        v.b = Decode(host, p->blue);
    } else {
        const PF_PixelFloat *p = WorldPixel<PF_PixelFloat>(world, x, y);
        v.a = p->alpha;
        v.r = p->red;
        v.g = p->green;
        v.b = p->blue;
    }
    return v;
}

bool PixelIs(TestHost &host, PF_EffectWorld &world, A_long x, A_long y, float r, float g,
             float b, float a, float tol_scale = 1.0f) {
    const PixelValues v = ReadPixel(host, world, x, y);
    float tol;
    if (host.bitdepth == 32) tol = 1e-6f * tol_scale;
    else tol = (host.bitdepth == 8 ? 0.5f / 255.0f : 0.5f / 32768.0f) * tol_scale;
    const bool ok = fabsf(v.r - r) <= tol && fabsf(v.g - g) <= tol &&
                    fabsf(v.b - b) <= tol && fabsf(v.a - a) <= tol;
    if (!ok)
        std::printf("  PixelIs(%ld,%ld) got a=%g r=%g g=%g b=%g want %g,%g,%g,%g\n",
                    (long)x, (long)y, v.a, v.r, v.g, v.b, r, g, b, a);
    return ok;
}

void FillRect(TestHost &host, int x0, int y0, int x1, int y1, float r, float g, float b,
              float a) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (x < 0 || y < 0 || x >= host.width || y >= host.height) continue;
            switch (host.bitdepth) {
                case 8: {
                    PF_Pixel *p = WorldPixel<PF_Pixel>(host.input, x, y);
                    p->red = (A_u_char)Encode(host, r);
                    p->green = (A_u_char)Encode(host, g);
                    p->blue = (A_u_char)Encode(host, b);
                    p->alpha = (A_u_char)Encode(host, a);
                    break;
                }
                case 16: {
                    PF_Pixel16 *p = WorldPixel<PF_Pixel16>(host.input, x, y);
                    p->red = (A_u_short)Encode(host, r);
                    p->green = (A_u_short)Encode(host, g);
                    p->blue = (A_u_short)Encode(host, b);
                    p->alpha = (A_u_short)Encode(host, a);
                    break;
                }
                default: {
                    PF_PixelFloat *p = WorldPixel<PF_PixelFloat>(host.input, x, y);
                    p->red = r;
                    p->green = g;
                    p->blue = b;
                    p->alpha = a;
                    break;
                }
            }
        }
    }
}

// --- host callbacks ----------------------------------------------------------

PF_Err add_param(PF_ProgPtr, PF_ParamIndex, PF_ParamDefPtr def) {
    if (!g_host || !def || g_host->add_count >= OLMS2_NUM_PARAMS - 1)
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    ++g_host->add_count;
    g_host->added[(size_t)g_host->add_count] = *def;
    g_host->params[(size_t)g_host->add_count] = *def;
    return PF_Err_NONE;
}

PF_Err checkout_param(PF_ProgPtr, PF_ParamIndex position, A_long, A_long, A_u_long,
                      PF_ParamDef *param) {
    if (!g_host || !param || position <= OLMS2_INPUT || position >= OLMS2_NUM_PARAMS)
        return PF_Err_BAD_CALLBACK_PARAM;
    ++g_host->param_checkouts;
    *param = g_host->params[(size_t)position];
    return PF_Err_NONE;
}

PF_Err checkin_param(PF_ProgPtr, PF_ParamDef *param) {
    if (!g_host || !param) return PF_Err_BAD_CALLBACK_PARAM;
    ++g_host->param_checkins;
    return PF_Err_NONE;
}

PF_Err copy_world(PF_ProgPtr, PF_EffectWorld *src, PF_EffectWorld *dst, PF_Rect *,
                  PF_Rect *) {
    if (!g_host || !src || !dst || !src->data || !dst->data) return PF_Err_BAD_CALLBACK_PARAM;
    const size_t row = (size_t)std::min(src->width, dst->width) *
                       (size_t)g_host->pixel_size;
    for (A_long y = 0; y < std::min(src->height, dst->height); ++y) {
        std::memcpy((char *)dst->data + (size_t)y * (size_t)dst->rowbytes,
                    (const char *)src->data + (size_t)y * (size_t)src->rowbytes, row);
    }
    return PF_Err_NONE;
}

int SPAPI mock_sprintf(A_char *buffer, const A_char *format, ...) {
    if (!g_host || !buffer || !format) return -1;
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(buffer, 256, format, args);
    va_end(args);
    return written;
}

PF_Err get_color(PF_ProgPtr, const PF_ParamDef *def, PF_PixelFloat *out) {
    if (!g_host || !def || !out) return PF_Err_BAD_CALLBACK_PARAM;
    ++g_host->color_suite_calls;
    out->red = (float)def->u.cd.value.red / 255.0f;
    out->green = (float)def->u.cd.value.green / 255.0f;
    out->blue = (float)def->u.cd.value.blue / 255.0f;
    out->alpha = (float)def->u.cd.value.alpha / 255.0f;
    return PF_Err_NONE;
}

PF_Err SPAPI update_param_ui(PF_ProgPtr, PF_ParamIndex position, const PF_ParamDef *def) {
    if (!g_host || !def) return PF_Err_BAD_CALLBACK_PARAM;
    // Reject zeroed definitions: the effect must pass the host's own live
    // def (type and name preserved) with only PF_PUI_DISABLED toggled.
    if (g_host->ui_reject_zeroed &&
        (def->param_type == 0 || def->PF_DEF_NAME[0] == '\0'))
        return PF_Err_BAD_CALLBACK_PARAM;
    g_host->ui_records.push_back({position, *def});
    return PF_Err_NONE;
}

PF_Err new_world(PF_ProgPtr, A_long width, A_long height, PF_Boolean clear,
                 PF_PixelFormat format, PF_EffectWorld *world) {
    if (!g_host || !world || width < 0 || height < 0) return PF_Err_BAD_CALLBACK_PARAM;
    const A_long px = format == PF_PixelFormat_ARGB32 ? 4
                      : format == PF_PixelFormat_ARGB64 ? 8
                                                        : 16;
    WorldBuffer *buf = new WorldBuffer();
    buf->rowbytes = width * px;
    buf->bytes.assign((size_t)buf->rowbytes * (size_t)height, 0);
    g_host->world_buffers.push_back(buf);
    AEFX_CLR_STRUCT(*world);
    world->data = (PF_PixelPtr)buf->bytes.data();
    world->rowbytes = buf->rowbytes;
    world->width = width;
    world->height = height;
    world->extent_hint.left = 0;
    world->extent_hint.top = 0;
    world->extent_hint.right = width;
    world->extent_hint.bottom = height;
    (void)clear;
    return PF_Err_NONE;
}

PF_Err dispose_world(PF_ProgPtr, PF_EffectWorld *world) {
    if (!g_host || !world) return PF_Err_BAD_CALLBACK_PARAM;
    for (size_t i = 0; i < g_host->world_buffers.size(); ++i) {
        if (g_host->world_buffers[i]->bytes.data() == (unsigned char *)world->data) {
            delete g_host->world_buffers[i];
            g_host->world_buffers.erase(g_host->world_buffers.begin() + (long)i);
            return PF_Err_NONE;
        }
    }
    return PF_Err_BAD_CALLBACK_PARAM;
}

PF_Err register_with_aegp(AEGP_GlobalRefcon, const A_char *, AEGP_PluginID *id) {
    if (!id) return (A_Err)1;
    *id = 0x1234;
    if (g_host) g_host->registered_id = *id;
    return A_Err_NONE;
}

PF_Err get_effect_layer(PF_ProgPtr, AEGP_LayerH *layer) {
    if (!layer) return (A_Err)1;
    *layer = (AEGP_LayerH)0x1;
    return A_Err_NONE;
}

PF_Err get_new_effect_for_effect(AEGP_PluginID, PF_ProgPtr, AEGP_EffectRefH *effect) {
    if (!effect) return (A_Err)1;
    *effect = (AEGP_EffectRefH)0x2;
    return A_Err_NONE;
}

PF_Err dispose_effect(AEGP_EffectRefH) {
    if (g_host) ++g_host->effect_disposes;
    return A_Err_NONE;
}

PF_Err get_stream_by_index(AEGP_PluginID, AEGP_EffectRefH, PF_ParamIndex index,
                           AEGP_StreamRefH *stream) {
    if (!stream) return (A_Err)1;
    *stream = (AEGP_StreamRefH)(ptrdiff_t)(index + 1);
    return A_Err_NONE;
}

PF_Err dispose_stream(AEGP_StreamRefH) {
    if (g_host) ++g_host->stream_disposes;
    return A_Err_NONE;
}

PF_Err set_dynamic_stream_flag(AEGP_StreamRefH stream, AEGP_DynStreamFlags flag,
                               A_Boolean undoable, A_Boolean set) {
    if (!g_host || flag != AEGP_DynStreamFlag_HIDDEN || undoable) return (A_Err)1;
    g_host->stream_records.push_back({(PF_ParamIndex)((ptrdiff_t)stream - 1), set});
    return A_Err_NONE;
}

SPErr SPAPI acquire_suite(const char *name, int32 version, const void **suite) {
    if (!g_host || !name || !suite) return (SPErr)-1;
    if (!std::strcmp(name, kPFColorParamSuite) && version == kPFColorParamSuiteVersion1)
        *suite = &g_host->color_suite;
    else if (!std::strcmp(name, kPFParamUtilsSuite) && version == kPFParamUtilsSuiteVersion3)
        *suite = &g_host->param_utils_suite;
    else if (!std::strcmp(name, kPFWorldSuite) && version == kPFWorldSuiteVersion2)
        *suite = &g_host->world_suite;
    else if (!std::strcmp(name, kAEGPUtilitySuite) && version == kAEGPUtilitySuiteVersion3)
        *suite = &g_host->util_suite;
    else if (!std::strcmp(name, kAEGPPFInterfaceSuite) &&
             version == kAEGPPFInterfaceSuiteVersion1)
        *suite = &g_host->pf_interface;
    else if (!std::strcmp(name, kAEGPEffectSuite) && version == kAEGPEffectSuiteVersion5)
        *suite = &g_host->effect_suite;
    else if (!std::strcmp(name, kAEGPStreamSuite) && version == kAEGPStreamSuiteVersion7)
        *suite = &g_host->stream_suite;
    else if (!std::strcmp(name, kAEGPDynamicStreamSuite) &&
             version == kAEGPDynamicStreamSuiteVersion3)
        *suite = &g_host->dyn_stream;
    else
        return (SPErr)-1;
    return kSPNoError;
}

SPErr SPAPI release_suite(const char *, int32) { return kSPNoError; }

PF_Err checkout_layer_pixels(PF_ProgPtr, A_long, PF_EffectWorld **world) {
    if (!g_host || !world) return PF_Err_BAD_CALLBACK_PARAM;
    ++g_host->layer_checkouts;
    *world = &g_host->input;
    return PF_Err_NONE;
}

PF_Err checkin_layer_pixels(PF_ProgPtr, A_long) {
    if (!g_host) return PF_Err_BAD_CALLBACK_PARAM;
    ++g_host->layer_checkins;
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
    if (!g_host || !result || position != OLMS2_INPUT || checkout_id != 0)
        return PF_Err_BAD_CALLBACK_PARAM;
    result->result_rect = g_host->input.extent_hint;
    result->max_result_rect = g_host->input.extent_hint;
    result->ref_width = g_host->in.width;
    result->ref_height = g_host->in.height;
    return PF_Err_NONE;
}

void set_world(PF_EffectWorld &world, std::vector<unsigned char> &bytes, A_long rowbytes,
               A_long width, A_long height) {
    AEFX_CLR_STRUCT(world);
    world.data = (PF_PixelPtr)bytes.data();
    world.rowbytes = rowbytes;
    world.width = width;
    world.height = height;
    world.origin_x = 0;
    world.origin_y = 0;
    world.extent_hint.left = 0;
    world.extent_hint.top = 0;
    world.extent_hint.right = width;
    world.extent_hint.bottom = height;
}

bool init_host(TestHost &host, int bitdepth, A_long width = kWidth,
               A_long height = kHeight) {
    g_host = &host;
    host.bitdepth = bitdepth;
    host.width = width;
    host.height = height;
    host.pixel_size = host.PixelSize();
    host.rowbytes = width * host.pixel_size;
    host.input_bytes.assign((size_t)host.rowbytes * (size_t)height, 0);
    host.output_bytes.assign((size_t)host.rowbytes * (size_t)height, 0);
    set_world(host.input, host.input_bytes, host.rowbytes, width, height);
    set_world(host.output, host.output_bytes, host.rowbytes, width, height);

    host.color_suite.PF_GetFloatingPointColorFromColorDef = get_color;
    host.param_utils_suite.PF_UpdateParamUI = update_param_ui;
    host.world_suite.PF_NewWorld = new_world;
    host.world_suite.PF_DisposeWorld = dispose_world;
    host.util_suite.AEGP_RegisterWithAEGP = register_with_aegp;
    host.pf_interface.AEGP_GetEffectLayer = get_effect_layer;
    host.pf_interface.AEGP_GetNewEffectForEffect = get_new_effect_for_effect;
    host.effect_suite.AEGP_DisposeEffect = dispose_effect;
    host.stream_suite.AEGP_GetNewEffectStreamByIndex = get_stream_by_index;
    host.stream_suite.AEGP_DisposeStream = dispose_stream;
    host.dyn_stream.AEGP_SetDynamicStreamFlag = set_dynamic_stream_flag;
    host.basic.AcquireSuite = acquire_suite;
    host.basic.ReleaseSuite = release_suite;
    host.utils.copy = copy_world;
    host.utils.ansi.sprintf = mock_sprintf;
    host.utils.ansi.strcpy = [](A_char *dst, const A_char *src) -> A_char * {
        return std::strcpy(dst, src);
    };

    host.in.inter.add_param = add_param;
    host.in.inter.checkout_param = checkout_param;
    host.in.inter.checkin_param = checkin_param;
    host.in.utils = &host.utils;
    host.in.effect_ref = (PF_ProgPtr)&host;
    host.in.pica_basicP = &host.basic;
    host.in.current_time = 0;
    host.in.time_step = 1;
    host.in.time_scale = 30;
    host.in.width = width;
    host.in.height = height;
    host.in.downsample_x.num = 1;
    host.in.downsample_x.den = 1;
    host.in.downsample_y.num = 1;
    host.in.downsample_y.den = 1;

    host.out.num_params = 0;
    if (EffectMain(PF_Cmd_GLOBAL_SETUP, &host.in, &host.out, nullptr, nullptr, nullptr) !=
        PF_Err_NONE)
        return false;
    if (EffectMain(PF_Cmd_PARAMS_SETUP, &host.in, &host.out, nullptr, nullptr, nullptr) !=
            PF_Err_NONE ||
        host.out.num_params != OLMS2_NUM_PARAMS || host.add_count != OLMS2_NUM_PARAMS - 1)
        return false;
    return true;
}

void teardown_host(TestHost &host) {
    for (size_t i = 0; i < host.world_buffers.size(); ++i) delete host.world_buffers[i];
    host.world_buffers.clear();
    g_host = nullptr;
}

bool run_pre_render(TestHost &host, const PF_Rect &request_rect, PF_Rect *result_out) {
    PF_PreRenderInput render_input;
    PF_PreRenderOutput render_output;
    PF_PreRenderCallbacks callbacks;
    PF_PreRenderExtra extra;
    AEFX_CLR_STRUCT(render_input);
    AEFX_CLR_STRUCT(render_output);
    AEFX_CLR_STRUCT(callbacks);
    AEFX_CLR_STRUCT(extra);
    render_input.output_request.rect = request_rect;
    render_input.bitdepth = (short)host.bitdepth;
    render_output.result_rect = request_rect;
    render_output.max_result_rect = request_rect;
    callbacks.checkout_layer = pre_render_checkout;
    extra.input = &render_input;
    extra.output = &render_output;
    extra.cb = &callbacks;
    const PF_Err err = EffectMain(PF_Cmd_SMART_PRE_RENDER, &host.in, &host.out, nullptr,
                                  nullptr, &extra);
    *result_out = render_output.result_rect;
    return err == PF_Err_NONE;
}

PF_Err run_render(TestHost &host, const PF_Rect &request_rect) {
    PF_SmartRenderInput render_input;
    PF_SmartRenderCallbacks callbacks;
    PF_SmartRenderExtra extra;
    AEFX_CLR_STRUCT(render_input);
    AEFX_CLR_STRUCT(callbacks);
    AEFX_CLR_STRUCT(extra);
    render_input.output_request.rect = request_rect;
    render_input.bitdepth = (short)host.bitdepth;
    callbacks.checkout_layer_pixels = checkout_layer_pixels;
    callbacks.checkin_layer_pixels = checkin_layer_pixels;
    callbacks.checkout_output = checkout_output;
    extra.input = &render_input;
    extra.cb = &callbacks;
    return EffectMain(PF_Cmd_SMART_RENDER, &host.in, &host.out, nullptr, nullptr, &extra);
}

void reset_output(TestHost &host) {
    std::memset(host.output_bytes.data(), 0, host.output_bytes.size());
}

bool RenderChecked(TestHost &host, const char *label) {
    reset_output(host);
    const int co_before = host.param_checkouts;
    const int ci_before = host.param_checkins;
    const int lc_before = host.layer_checkouts;
    const int li_before = host.layer_checkins;
    const int oc_before = host.output_checkouts;
    PF_Rect request{0, 0, host.width, host.height};
    PF_Rect result{0, 0, 0, 0};
    if (!run_pre_render(host, request, &result)) {
        std::printf("FAIL: %s pre-render errored\n", label);
        ++g_failures;
        return false;
    }
    CHECK(result.left == 0 && result.top == 0 && result.right == host.width &&
              result.bottom == host.height,
          "pre-render should union to the full input extent");
    const PF_Err err = run_render(host, request);
    if (err != PF_Err_NONE) {
        std::printf("FAIL: %s render errored (%d)\n", label, (int)err);
        ++g_failures;
        return false;
    }
    CHECK(host.param_checkouts - co_before == host.param_checkins - ci_before &&
              host.layer_checkouts - lc_before == 1 &&
              host.layer_checkins - li_before == 1 &&
              host.output_checkouts - oc_before == 1,
          "checkout/checkin balance");
    return true;
}

bool SameString(const A_char *a, const char *b) { return std::strcmp(a, b) == 0; }

// The value a truncated 8/16-bit channel decodes back to (the port routes
// every pixel through the float canvas, so outputs are quantized).
float EncDec(TestHost &host, float v) {
    return Decode(host, Encode(host, v));
}

bool CheckFloatSlider(const PF_ParamDef &def, const char *name, double vmin, double vmax,
                      double smin, double smax, double dflt, int prec, A_long id) {
    // PF_FpShort is float: compare the non-exact decimals with tolerance.
    const auto near_eq = [](double a, double b) { return std::fabs(a - b) < 1e-4; };
    if (def.uu.id == id && def.param_type == PF_Param_FLOAT_SLIDER &&
        SameString(def.PF_DEF_NAME, name) && near_eq(def.u.fs_d.valid_min, vmin) &&
        near_eq(def.u.fs_d.valid_max, vmax) && near_eq(def.u.fs_d.slider_min, smin) &&
        near_eq(def.u.fs_d.slider_max, smax) && near_eq(def.u.fs_d.dephault, dflt) &&
        (int)def.u.fs_d.precision == prec)
        return true;
    std::printf("  diag: id=%d want=%d type=%d name='%s' want='%s' vmin=%g vmax=%g "
                "smin=%g smax=%g dflt=%g prec=%d\n",
                (int)def.uu.id, (int)id, (int)def.param_type, def.PF_DEF_NAME, name,
                (double)def.u.fs_d.valid_min, (double)def.u.fs_d.valid_max,
                (double)def.u.fs_d.slider_min, (double)def.u.fs_d.slider_max,
                (double)def.u.fs_d.dephault, (int)def.u.fs_d.precision);
    return false;
}

bool verify_setup() {
    TestHost host;
    if (!init_host(host, 8)) {
        std::printf("FAIL: host init / params setup\n");
        ++g_failures;
        return false;
    }
    CHECK(host.out.num_params == 16, "num_params == 16");
    CHECK(host.add_count == 15, "15 params added");

    const PF_ParamDef &enable = host.added[1];
    CHECK(enable.uu.id == 1 && enable.param_type == PF_Param_CHECKBOX &&
              SameString(enable.PF_DEF_NAME, "Enable Color Key") &&
              enable.u.bd.dephault == 0,
          "pos1 Enable Color Key checkbox");
    {
        const PF_ParamDef &def = host.added[2];
        CHECK(def.uu.id == 2 && def.param_type == PF_Param_COLOR &&
                  SameString(def.PF_DEF_NAME, "Color Key") &&
                  def.u.cd.value.red == 255 && def.u.cd.value.green == 255 &&
                  def.u.cd.value.blue == 255 && def.u.cd.value.alpha == 255 &&
                  def.u.cd.dephault.red == 255,
              "pos2 Color Key white");
    }
    CHECK(host.added[3].uu.id == 15 && host.added[3].param_type == PF_Param_CHECKBOX &&
              SameString(host.added[3].PF_DEF_NAME, "Invert Color Key") &&
              host.added[3].u.bd.dephault == 0,
          "pos3 Invert Color Key (id 15)");
    {
        const PF_ParamDef &def = host.added[4];
        CHECK(def.uu.id == 3 && def.param_type == PF_Param_SLIDER &&
                  SameString(def.PF_DEF_NAME, "Smoothness") &&
                  def.u.sd.valid_min == 0 && def.u.sd.valid_max == 100 &&
                  def.u.sd.slider_min == 0 && def.u.sd.slider_max == 100 &&
                  def.u.sd.dephault == 100,
              "pos4 Smoothness 0..100 dflt 100 (id 3)");
    }
    {
        const PF_ParamDef &def = host.added[5];
        CHECK(def.uu.id == 4 && def.param_type == PF_Param_SLIDER &&
                  SameString(def.PF_DEF_NAME, "Extra Smooth") &&
                  def.u.sd.valid_min == 0 && def.u.sd.valid_max == 100 &&
                  def.u.sd.slider_min == 0 && def.u.sd.slider_max == 100 &&
                  def.u.sd.dephault == 0,
              "pos5 Extra Smooth 0..100 dflt 0 (id 4)");
    }
    {
        const PF_ParamDef &def = host.added[6];
        CHECK(def.uu.id == 5 && def.param_type == PF_Param_SLIDER &&
                  SameString(def.PF_DEF_NAME, "Smooth Range") &&
                  def.u.sd.valid_min == 0 && def.u.sd.valid_max == 100 &&
                  def.u.sd.slider_min == 0 && def.u.sd.slider_max == 100 &&
                  def.u.sd.dephault == 2,
              "pos6 Smooth Range 0..100 dflt 2 (id 5)");
    }
    {
        const PF_ParamDef &def = host.added[7];
        CHECK(def.uu.id == 6 && def.param_type == PF_Param_POPUP &&
                  def.u.pd.num_choices == 2 && def.u.pd.dephault == 2 &&
                  SameString(def.PF_DEF_NAME, "Smoother Version") &&
                  SameString(def.u.pd.u.PF_DEF_NAMESPTR, "v1|v2"),
              "pos7 Smoother Version v1|v2 dflt 2 (id 6)");
    }
    {
        const PF_ParamDef &def = host.added[8];
        CHECK(def.uu.id == 7 && def.param_type == PF_Param_POPUP &&
                  def.u.pd.num_choices == 3 && def.u.pd.dephault == 1 &&
                  SameString(def.PF_DEF_NAME, "Gamma Correction") &&
                  SameString(def.u.pd.u.PF_DEF_NAMESPTR, "None|Gamma Colors|All Colors") &&
                  (def.flags & PF_ParamFlag_SUPERVISE),
              "pos8 Gamma Correction dflt None SUPERVISE (id 7)");
    }
    CHECK(CheckFloatSlider(host.added[9], "Gamma Value", 1.0, 4.8, 1.0, 4.8, 2.4, 2, 8),
          "pos9 Gamma Value 1..4.8 dflt 2.4 prec 2 (id 8)");
    {
        const PF_ParamDef &def = host.added[10];
        CHECK(def.uu.id == 9 && def.param_type == PF_Param_SLIDER &&
                  SameString(def.PF_DEF_NAME, "Number of Gamma Colors") &&
                  def.u.sd.valid_min == 0 && def.u.sd.valid_max == 5 &&
                  def.u.sd.slider_min == 0 && def.u.sd.slider_max == 5 &&
                  def.u.sd.dephault == 1 && (def.flags & PF_ParamFlag_SUPERVISE),
              "pos10 Number of Gamma Colors 0..5 dflt 1 SUPERVISE (id 9)");
    }
    for (int i = 0; i < OLMS2_MAX_GAMMA_COLORS; ++i) {
        const PF_ParamDef &def = host.added[(size_t)(11 + i)];
        char msg[128];
        std::snprintf(msg, sizeof(msg), "pos%d Gamma Color black (id %d)", 11 + i, 10 + i);
        CHECK(def.uu.id == 10 + i && def.param_type == PF_Param_COLOR &&
                  SameString(def.PF_DEF_NAME, "Gamma Color") &&
                  def.u.cd.value.red == 0 && def.u.cd.value.green == 0 &&
                  def.u.cd.value.blue == 0 && def.u.cd.value.alpha == 255 &&
                  def.u.cd.dephault.alpha == 255,
              msg);
    }
    CHECK(host.added[9].uu.id == 8 && host.added[8].uu.id == 7,
          "position index differs from uu.id");

    CHECK(host.out.my_version == 1081344,
          "my_version == PF_VERSION(2,1,0,DEVELOP,0) = 1081344");
    CHECK(host.out.out_flags == 0x06000440,
          "out_flags 0x06000440 (binary 0x02000440 + SEND_UPDATE_PARAMS_UI)");
    CHECK(host.out.out_flags2 == 0x08001400, "out_flags2 0x08001400");
    CHECK(host.registered_id == 0x1234, "AEGP_RegisterWithAEGP called");

    {
        PF_OutData about_out;
        AEFX_CLR_STRUCT(about_out);
        EffectMain(PF_Cmd_ABOUT, &host.in, &about_out, nullptr, nullptr, nullptr);
        CHECK(SameString(about_out.return_msg, "OLM Smoother v2 2.1\rSmooth images."),
              "About text");
    }

    // USER_CHANGED_PARAM must be a no-op.
    PF_ParamDef *params[OLMS2_NUM_PARAMS] = {};
    for (int i = 0; i < OLMS2_NUM_PARAMS; ++i) params[i] = &host.params[(size_t)i];
    PF_Err ucp = EffectMain(PF_Cmd_USER_CHANGED_PARAM, &host.in, &host.out, params,
                            nullptr, nullptr);
    CHECK(ucp == PF_Err_NONE, "USER_CHANGED_PARAM no-op");

    teardown_host(host);
    std::printf("setup: 16 params, identities, flags, version, about OK\n");
    return true;
}

// --- 2. UPDATE_PARAMS_UI -----------------------------------------------------

struct UiExpect {
    std::map<PF_ParamIndex, bool> ui_disabled;    // PF_UpdateParamUI records
    std::map<PF_ParamIndex, bool> stream_hidden;  // SetDynamicStreamFlag records
};

void BuildUiExpect(UiExpect &e, bool enable, int mode, A_long count) {
    e.ui_disabled.clear();
    e.stream_hidden.clear();
    e.ui_disabled[OLMS2_KEY_COLOR] = !enable;
    e.ui_disabled[OLMS2_INVERT_KEY] = !enable;
    e.ui_disabled[OLMS2_GAMMA_VALUE] = (mode == OLMS2_GAMMA_NONE);
    e.ui_disabled[OLMS2_GAMMA_COUNT] = (mode != OLMS2_GAMMA_COLORS);
    for (int i = 0; i < OLMS2_MAX_GAMMA_COLORS; ++i) {
        e.stream_hidden[OLMS2_GAMMA_COLOR_0 + i] =
            !(mode == OLMS2_GAMMA_COLORS && i < count);
    }
}

bool VerifyUiRecords(TestHost &host, const UiExpect &e) {
    if (host.ui_records.size() != e.ui_disabled.size()) {
        std::printf("FAIL: UI expected %lu UpdateParamUI records, got %lu\n",
                    (unsigned long)e.ui_disabled.size(),
                    (unsigned long)host.ui_records.size());
        ++g_failures;
        return false;
    }
    for (size_t i = 0; i < host.ui_records.size(); ++i) {
        const PF_ParamIndex position = host.ui_records[i].position;
        std::map<PF_ParamIndex, bool>::const_iterator it = e.ui_disabled.find(position);
        if (it == e.ui_disabled.end()) {
            std::printf("FAIL: unexpected UpdateParamUI for position %d\n", (int)position);
            ++g_failures;
            return false;
        }
        PF_ParamDef expected = host.params[(size_t)position];
        if (it->second) expected.ui_flags |= PF_PUI_DISABLED;
        else expected.ui_flags &= ~((A_long)PF_PUI_DISABLED);
        if (std::memcmp(&host.ui_records[i].def, &expected, sizeof(PF_ParamDef)) != 0) {
            std::printf("FAIL: UpdateParamUI def mismatch at %d (must be the host's "
                        "definition with only PF_PUI_DISABLED toggled)\n",
                        (int)position);
            ++g_failures;
            return false;
        }
    }
    if (host.stream_records.size() != e.stream_hidden.size()) {
        std::printf("FAIL: UI expected %lu stream records, got %lu\n",
                    (unsigned long)e.stream_hidden.size(),
                    (unsigned long)host.stream_records.size());
        ++g_failures;
        return false;
    }
    for (size_t i = 0; i < host.stream_records.size(); ++i) {
        std::map<PF_ParamIndex, bool>::const_iterator it =
            e.stream_hidden.find(host.stream_records[i].position);
        if (it == e.stream_hidden.end() || it->second != host.stream_records[i].hidden) {
            std::printf("FAIL: stream hidden mismatch at position %d\n",
                        (int)host.stream_records[i].position);
            ++g_failures;
            return false;
        }
    }
    if (host.effect_disposes != 1 ||
        host.stream_disposes != (int)e.stream_hidden.size()) {
        std::printf("FAIL: UI AEGP dispose balance (%d effects, %d streams)\n",
                    host.effect_disposes, host.stream_disposes);
        ++g_failures;
        return false;
    }
    return true;
}

bool RunUiMode(TestHost &host, bool enable, int mode, A_long count, const char *label) {
    host.params[OLMS2_ENABLE_KEY].u.bd.value = enable ? 1 : 0;
    host.params[OLMS2_GAMMA_MODE].u.pd.value = (A_long)mode;
    host.params[OLMS2_GAMMA_COUNT].u.sd.value = count;
    host.ui_records.clear();
    host.stream_records.clear();
    host.effect_disposes = 0;
    host.stream_disposes = 0;
    std::vector<PF_ParamDef *> params((size_t)OLMS2_NUM_PARAMS);
    for (int i = 0; i < OLMS2_NUM_PARAMS; ++i) params[(size_t)i] = &host.params[(size_t)i];
    const PF_Err err = EffectMain(PF_Cmd_UPDATE_PARAMS_UI, &host.in, &host.out,
                                  params.data(), nullptr, nullptr);
    if (err != PF_Err_NONE) {
        std::printf("FAIL: UPDATE_PARAMS_UI errored in %s (%d)\n", label, (int)err);
        ++g_failures;
        return false;
    }
    UiExpect e;
    BuildUiExpect(e, enable, mode, count);
    if (!VerifyUiRecords(host, e)) return false;
    std::printf("UI %s: %lu disable + %lu stream records OK\n", label,
                (unsigned long)host.ui_records.size(),
                (unsigned long)host.stream_records.size());
    return true;
}

bool verify_ui() {
    TestHost host;
    if (!init_host(host, 8)) {
        std::printf("FAIL: UI host init\n");
        ++g_failures;
        return false;
    }
    if (!RunUiMode(host, false, OLMS2_GAMMA_NONE, 1, "off/None")) return false;
    if (!RunUiMode(host, true, OLMS2_GAMMA_NONE, 1, "on/None")) return false;
    if (!RunUiMode(host, true, OLMS2_GAMMA_COLORS, 2, "on/GammaColors/2")) return false;
    if (!RunUiMode(host, true, OLMS2_GAMMA_ALL, 5, "on/AllColors")) return false;
    if (!RunUiMode(host, false, OLMS2_GAMMA_COLORS, 3, "off/GammaColors/3")) return false;

    // Null params[] must be rejected.
    PF_Err err = EffectMain(PF_Cmd_UPDATE_PARAMS_UI, &host.in, &host.out, nullptr,
                            nullptr, nullptr);
    CHECK(err == PF_Err_BAD_CALLBACK_PARAM, "null params[] -> BAD_CALLBACK_PARAM");

    // Zeroed defs would be rejected by the mock: run one mode with rejection
    // armed; the effect must still succeed because it passes full copies.
    host.ui_reject_zeroed = true;
    if (!RunUiMode(host, true, OLMS2_GAMMA_COLORS, 2, "on/GammaColors/2/reject-zeroed"))
        return false;
    host.ui_reject_zeroed = false;

    teardown_host(host);
    return true;
}

// --- 3. render scenarios -----------------------------------------------------

bool ScenarioPassthrough(TestHost &host, int depth) {
    if (!init_host(host, depth)) {
        std::printf("FAIL: passthrough host init (%d)\n", depth);
        ++g_failures;
        return false;
    }
    // Varied image, including mid-gray; v1 + smoothness 0 -> exact copy.
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const float v = (float)((x * 7 + y * 11) % 256) / 255.0f;
            FillRect(host, x, y, x, y, v, 1.0f - v, v * 0.5f, (float)((x + y) % 2));
        }
    }
    host.params[OLMS2_VERSION].u.pd.value = 1;  // v1
    host.params[OLMS2_SMOOTHNESS].u.sd.value = 0;
    if (!RenderChecked(host, "v1 passthrough")) return false;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const PixelValues i = ReadPixel(host, host.input, x, y);
            if (!PixelIs(host, host.output, x, y, i.r, i.g, i.b, i.a)) {
                std::printf("FAIL: v1 passthrough mismatch at %ld,%ld\n", (long)x, (long)y);
                ++g_failures;
                return false;
            }
        }
    }
    std::printf("v1 passthrough depth %d OK\n", depth);
    teardown_host(host);
    return true;
}

bool ScenarioV2Roundtrip(TestHost &host, int depth) {
    if (!init_host(host, depth)) {
        std::printf("FAIL: v2 host init (%d)\n", depth);
        ++g_failures;
        return false;
    }
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 0.5f, 0.5f, 0.5f, 1.0f);
    host.params[OLMS2_VERSION].u.pd.value = 2;  // v2 (decode + encode LUTs)
    host.params[OLMS2_SMOOTHNESS].u.sd.value = 0;
    if (!RenderChecked(host, "v2 roundtrip")) return false;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const PixelValues i = ReadPixel(host, host.input, x, y);
            // 8-bit may change by up to one code from the LUT roundtrip;
            // assert it runs and stays within one code of the input.
            if (!PixelIs(host, host.output, x, y, i.r, i.g, i.b, i.a,
                         depth == 8 ? 2.0f : 1.0f)) {
                std::printf("FAIL: v2 roundtrip off by >1 code at %ld,%ld\n",
                            (long)x, (long)y);
                ++g_failures;
                return false;
            }
        }
    }
    std::printf("v2 roundtrip depth %d OK\n", depth);
    teardown_host(host);
    return true;
}

bool ScenarioKey(TestHost &host, int depth, bool invert) {
    if (!init_host(host, depth)) {
        std::printf("FAIL: key host init (%d)\n", depth);
        ++g_failures;
        return false;
    }
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 0.25f, 0.5f, 0.75f, 1.0f);
    FillRect(host, 2, 2, 4, 4, 1.0f, 1.0f, 1.0f, 1.0f);       // opaque white
    FillRect(host, 6, 1, 7, 2, 1.0f, 1.0f, 1.0f, 0.5f);       // partial-alpha white
    host.params[OLMS2_VERSION].u.pd.value = 1;
    host.params[OLMS2_SMOOTHNESS].u.sd.value = 0;
    host.params[OLMS2_ENABLE_KEY].u.bd.value = 1;
    host.params[OLMS2_INVERT_KEY].u.bd.value = invert ? 1 : 0;
    if (!RenderChecked(host, invert ? "key invert" : "key")) return false;
    // White (any alpha) matches on RGB only; alpha is not compared. Under
    // invert the matched pixel keeps its own alpha; without invert it goes
    // to alpha 0. RGB is preserved in both cases. Values pass through the
    // float canvas, so compare against the quantized fill values.
    const float wr = EncDec(host, 1.0f), wg = EncDec(host, 1.0f), wb = EncDec(host, 1.0f);
    const float br = EncDec(host, 0.25f), bg = EncDec(host, 0.5f), bb = EncDec(host, 0.75f);
    const float white_a = invert ? EncDec(host, 1.0f) : 0.0f;
    const float partial_a = invert ? EncDec(host, 0.5f) : 0.0f;
    const float kept_a = invert ? 0.0f : EncDec(host, 1.0f);
    CHECK(PixelIs(host, host.output, 3, 3, wr, wg, wb, white_a),
          invert ? "opaque white kept under invert" : "opaque white keyed out");
    CHECK(PixelIs(host, host.output, 6, 1, wr, wg, wb, partial_a),
          invert ? "partial-alpha white keeps its own alpha"
                 : "partial-alpha white keyed out");
    if (invert) {
        CHECK(PixelIs(host, host.output, 0, 0, br, bg, bb, 0.0f),
              "non-white keyed out under invert");
        CHECK(PixelIs(host, host.output, 5, 5, br, bg, bb, 0.0f),
              "non-white keyed out under invert 2");
    } else {
        CHECK(PixelIs(host, host.output, 0, 0, br, bg, bb, kept_a),
              "non-white kept, alpha kept");
        CHECK(PixelIs(host, host.output, 5, 5, br, bg, bb, kept_a),
              "RGB preserved on kept pixels");
    }
    std::printf("key depth %d invert %d OK\n", depth, invert ? 1 : 0);
    teardown_host(host);
    return true;
}

// --- 4. edge distance / edge bytes (direct internal calls) -------------------

bool verify_edges() {
    // One unit step of 0.5 in one channel exceeds thr = 2/100 + 0.001.
    {
        const float a[4] = {0.5f, 0.0f, 0.0f, 1.0f};
        const float b[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        CHECK(EdgeDistance(a, b) >= 0.021f, "0.5 step exceeds thr");
        CHECK(EdgeDistance(a, b) >= (float)(2 / 100.0) + 0.001f, "thr value");
    }
    {
        const float a[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const float b[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        // |dA| = 1 -> distance 1: alpha differences count.
        CHECK(EdgeDistance(a, b) >= 0.021f, "alpha-only step counts");
    }
    {
        const float a[4] = {1.0f, 1.0f, 1.0f, 0.0f};
        const float b[4] = {1.0f, 1.0f, 1.0f, 0.0f};
        CHECK(EdgeDistance(a, b) == 0.0f, "alpha-0 vs alpha-0 distance is 0");
    }
    {
        const float a[4] = {0.0f, 0.5f, 0.0f, 1.0f};
        const float b[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        // luma weights: 0.5*0.7152 = 0.3576 > thr -> present via luma too.
        CHECK(EdgeDistance(a, b) >= 0.3576f, "luma term present");
    }

    // Edge-byte layout on a 2x2 half/half image.
    CanvasDesc c;
    std::vector<float> px(2 * 2 * 4);
    c.data = px.data();
    c.w = 2;
    c.h = 2;
    c.stride = 2 * 16;
    // row 0 white, row 1 black (R,G,B,A order).
    for (int x = 0; x < 2; ++x) {
        float *top = c.Pixel(x, 0);
        top[0] = top[1] = top[2] = 1.0f;
        top[3] = 1.0f;
        float *bot = c.Pixel(x, 1);
        bot[0] = bot[1] = bot[2] = 0.0f;
        bot[3] = 1.0f;
    }
    EdgeDesc e;
    std::vector<A_u_char> eb(2 * 2 * 4, 0xEE);
    e.data = eb.data();
    e.w = 2;
    e.h = 2;
    e.stride = 2 * 4;
    BuildEdges(c, e, 2);
    // (0,0): nothing (y=0 row has no up edges; x=0 no left).
    CHECK(e.Byte(0, 0, 0) == 0 && e.Byte(0, 0, 1) == 0 && e.Byte(0, 0, 2) == 0 &&
              e.Byte(0, 0, 3) == 0,
          "corner (0,0) has no edges");
    CHECK(e.Byte(1, 0, 0) == 0 && e.Byte(1, 0, 1) == 0 && e.Byte(1, 0, 2) == 0 &&
              e.Byte(1, 0, 3) == 0,
          "top row has no edges");
    // (0,1): b1 up present (white above, black here); b0 absent (x=0); b2 absent; b3 needs x+1 < W-1.
    CHECK(e.Byte(0, 1, 0) == 0 && e.Byte(0, 1, 1) == 0xFF && e.Byte(0, 1, 2) == 0 &&
              e.Byte(0, 1, 3) == 0,
          "(0,1) only b1 up");
    // (1,1): b1 up present; b2 up-left present; b0 absent (same row); b3 needs x+1=2 < W-1=1 -> no.
    CHECK(e.Byte(1, 1, 0) == 0 && e.Byte(1, 1, 1) == 0xFF && e.Byte(1, 1, 2) == 0xFF &&
              e.Byte(1, 1, 3) == 0,
          "(1,1) b1+b2");
    std::printf("edge distance + byte layout OK\n");
    return true;
}

// --- 5. family A image: the run-gated corner blend ---------------------------
// Image (5x3): rows 0-1 = [W B B B B], row 2 = [W W W W W] (opaque).
// v1, Smoothness 100, Extra Smooth 0, Range 2, no key, gamma None.
// Only pixel (1,1) blends: idx 22 -> RunGatedDownLeft at full s. Its searches
// find runs 3 (right, D8C0 ends at x=3 type 0) and 2 (up, CFA0 ends at y=0),
// and b2(1,2) is absent, so the binary appends (0x180012E60): mn = min(3,2)
// = 2 -> base 0.5 (not the 0.125 short-run base); k = 0.5 * s; weights
// 0.4k/0.2k/0.4k = 0.2/0.1/0.2, wsum = 0.5. Blend of the black pixel with the
// three white corner samples = 0.5 gray, alpha stays 1.
bool ScenarioFamilyA(TestHost &host, int depth) {
    if (!init_host(host, depth, 5, 3)) {
        std::printf("FAIL: familyA host init (%d)\n", depth);
        ++g_failures;
        return false;
    }
    FillRect(host, 1, 0, 4, 1, 0.0f, 0.0f, 0.0f, 1.0f);  // black block (cols 1..4, rows 0..1)
    // (col 0 and row 2 stay white from the zeroed buffer = 0; set white explicitly)
    FillRect(host, 0, 0, 0, 2, 1.0f, 1.0f, 1.0f, 1.0f);
    FillRect(host, 1, 2, 4, 2, 1.0f, 1.0f, 1.0f, 1.0f);

    host.params[OLMS2_VERSION].u.pd.value = 1;
    host.params[OLMS2_SMOOTHNESS].u.sd.value = 100;
    host.params[OLMS2_EXTRA_SMOOTH].u.sd.value = 0;
    host.params[OLMS2_SMOOTH_RANGE].u.sd.value = 2;
    if (!RenderChecked(host, "family A")) return false;

    // Independent corner-weight computation (the run-gated 0.4/0.2/0.4 split
    // at 0.5 base, s = 1, full s_scale): wsum = 0.5, so a black pixel blended
    // with white samples lands at exactly 0.5.
    const float wsum = (0.4f + 0.2f + 0.4f) * (0.5f * 1.0f * 1.0f);
    CHECK(wsum == 0.5f, "corner weight sums to 0.5");

    const float expect = EncDec(host, wsum);  // 0.5 quantizes to code 128
    if (depth == 8) {
        CHECK(PixelIs(host, host.output, 1, 1, expect, expect, expect, 1.0f),
              "run-gated corner blend at (1,1) equals the 0.5 mix (8-bit)");
        const float expect_decoded =
            (float)(A_u_char)(int)((wsum * 255.0f) + 0.5f) * 0.0039215689f;
        CHECK(ReadPixel(host, host.output, 1, 1).r == expect_decoded,
              "corner writer rounding (int)(v*255+0.5)");
    } else if (depth == 16) {
        CHECK(PixelIs(host, host.output, 1, 1, expect, expect, expect, 1.0f),
              "run-gated corner blend at (1,1) equals the 0.5 mix (16-bit)");
    } else {
        CHECK(PixelIs(host, host.output, 1, 1, expect, expect, expect, 1.0f),
              "run-gated corner blend at (1,1) equals the 0.5 mix (32-bit)");
    }
    // Neighbours must be untouched (in particular (2,1), the old scenario's
    // blend pixel: the corrected end-type classification leaves it alone).
    CHECK(PixelIs(host, host.output, 2, 1, 0, 0, 0, 1), "(2,1) untouched");
    CHECK(PixelIs(host, host.output, 3, 1, 0, 0, 0, 1), "(3,1) untouched");
    CHECK(PixelIs(host, host.output, 4, 1, 0, 0, 0, 1), "(4,1) untouched");
    CHECK(PixelIs(host, host.output, 0, 1, 1, 1, 1, 1), "(0,1) untouched");
    CHECK(PixelIs(host, host.output, 1, 0, 0, 0, 0, 1), "(1,0) untouched");
    CHECK(PixelIs(host, host.output, 1, 2, 1, 1, 1, 1), "(1,2) untouched");
    CHECK(PixelIs(host, host.output, 2, 2, 1, 1, 1, 1), "(2,2) untouched");
    std::printf("run-gated corner blend depth %d OK (wsum=%g)\n", depth, (double)wsum);
    teardown_host(host);
    return true;
}

// --- 6. isolated diagonal corner: all four corners at k = 0.125 --------------

bool ScenarioCorner(TestHost &host, int depth) {
    if (!init_host(host, depth, 5, 5)) {
        std::printf("FAIL: corner host init (%d)\n", depth);
        ++g_failures;
        return false;
    }
    FillRect(host, 0, 0, 4, 4, 1.0f, 1.0f, 1.0f, 1.0f);
    FillRect(host, 2, 2, 2, 2, 0.0f, 0.0f, 0.0f, 1.0f);  // single black pixel
    host.params[OLMS2_VERSION].u.pd.value = 1;
    host.params[OLMS2_SMOOTHNESS].u.sd.value = 100;
    host.params[OLMS2_EXTRA_SMOOTH].u.sd.value = 0;
    if (!RenderChecked(host, "corner")) return false;

    // k = 0.125 * s = 0.125; the 12 corner samples are all white with
    // total weight 4*0.8k + 4*0.2k = 4k = 0.5.
    // out = (1 - 0.5) * black + 0.5 * white = 0.5.
    const float k = 0.125f * 1.0f;
    const float wsum = 4.0f * (k * 0.40000001f + k * 0.40000001f) +
                       4.0f * (k * 0.2f);
    const float expect = (1.0f - wsum) * 0.0f + wsum * 1.0f;
    const float expect_q = EncDec(host, expect);
    CHECK(PixelIs(host, host.output, 2, 2, expect_q, expect_q, expect_q, 1.0f),
          "corner mix at (2,2) equals the 0.4/0.2/0.4 sum");
    CHECK(PixelIs(host, host.output, 1, 2, 1, 1, 1, 1), "(1,2) untouched");
    CHECK(PixelIs(host, host.output, 2, 1, 1, 1, 1, 1), "(2,1) untouched");
    CHECK(PixelIs(host, host.output, 3, 2, 1, 1, 1, 1), "(3,2) untouched");
    CHECK(PixelIs(host, host.output, 2, 3, 1, 1, 1, 1), "(2,3) untouched");
    std::printf("corner mix depth %d OK (sum=%g)\n", depth, (double)wsum);
    teardown_host(host);
    return true;
}

// --- 7. gamma All Colors on the family-A blend -------------------------------

bool ScenarioGamma(TestHost &host, int depth) {
    if (!init_host(host, depth, 5, 3)) {
        std::printf("FAIL: gamma host init (%d)\n", depth);
        ++g_failures;
        return false;
    }
    FillRect(host, 1, 0, 4, 1, 0.0f, 0.0f, 0.0f, 1.0f);
    FillRect(host, 0, 0, 0, 2, 1.0f, 1.0f, 1.0f, 1.0f);
    FillRect(host, 1, 2, 4, 2, 1.0f, 1.0f, 1.0f, 1.0f);

    host.params[OLMS2_VERSION].u.pd.value = 2;       // v2: linear canvas
    host.params[OLMS2_SMOOTHNESS].u.sd.value = 100;
    host.params[OLMS2_EXTRA_SMOOTH].u.sd.value = 0;
    host.params[OLMS2_GAMMA_MODE].u.pd.value = OLMS2_GAMMA_ALL;
    host.params[OLMS2_GAMMA_VALUE].u.fs_d.value = 2.4;
    if (!RenderChecked(host, "gamma all colors")) return false;

    // Same corner blend as ScenarioFamilyA (wsum = 0.5 of white into black),
    // but the blend happens on linear values (black=0, white=1) -> result =
    // 0.5 linear; then B1E0 applies pow(v, 2.4) and the writer encodes to sRGB.
    const float wsum = (0.4f + 0.2f + 0.4f) * (0.5f * 1.0f * 1.0f);
    const double linear = (double)wsum;
    const double gammaed = std::pow(linear, 2.4);
    const double enc = gammaed < 0.0031308
                           ? gammaed * 12.92
                           : std::pow(gammaed, 1.0 / 2.4) * 1.055 - 0.055;
    if (depth == 8) {
        const float want = (float)enc;
        const PixelValues got = ReadPixel(host, host.output, 1, 1);
        const float tol = 1.5f / 255.0f;  // LUT lerp + encode rounding slack
        CHECK(fabsf(got.r - want) <= tol && fabsf(got.g - want) <= tol &&
                  fabsf(got.b - want) <= tol && got.a == 1.0f,
              "gamma All Colors blend matches pow(2.4) + sRGB encode");
        std::printf("gamma blend: got %g want %g\n", (double)got.r, (double)enc);
    } else {
        const float want = (float)enc;
        const PixelValues got = ReadPixel(host, host.output, 1, 1);
        const float tol = depth == 16 ? 2.0f / 32768.0f : 1e-6f;
        CHECK(fabsf(got.r - want) <= tol && fabsf(got.g - want) <= tol &&
                  fabsf(got.b - want) <= tol,
              "gamma All Colors blend matches (16/32-bit)");
    }
    std::printf("gamma All Colors depth %d OK\n", depth);
    teardown_host(host);
    return true;
}

// --- 8. stair weight math (mandated translations 0x1800137C0/13C80/12910) ----

bool verify_stair_math() {
    // A 45-degree stair is translation-invariant: every interior step cuts
    // the same half-pixel triangle (legs 0.5 -> area 0.125) on each side.
    // Case 10 (137C0 over {1.0,0.5} -> {h+1, h+0.5}) must give the constant
    // 0.125/0.125 for interior steps and drop the end weight at the last
    // step (guard k+2 > P2.x).
    for (int h = 4; h <= 9; ++h) {
        for (int k = 0; k <= h + 1; ++k) {
            float w[2];
            StairWeights12910(w, h, k, 0, 10);
            CHECK(w[0] == 0.125f, "stair case 10 start-side weight is 0.125");
            if (k <= h - 1) {
                CHECK(w[1] == 0.125f, "stair case 10 interior end weight is 0.125");
            } else {
                CHECK(w[1] == 0.0f, "stair case 10 end weight 0 past the segment");
            }
            // Case 0 (offset half pixel) shares the invariant.
            StairWeights12910(w, h, k, 0, 0);
            if (k >= 1 && k <= h - 2) {
                CHECK(w[0] == 0.125f && w[1] == 0.125f,
                      "stair case 0 interior weights are 0.125/0.125");
            }
        }
    }
    // All 16 cases: weights are bounded area fractions in [0, 1].
    for (int h = 5; h <= 9; ++h) {
        for (int k = 0; k <= h + 1; ++k) {
            for (int which = 0; which <= 15; ++which) {
                float w[2];
                StairWeights12910(w, h, k, which % 4, which / 4);
                CHECK(w[0] >= 0.0f && w[0] <= 1.0f && w[1] >= 0.0f && w[1] <= 1.0f,
                      "stair weights are area fractions in [0,1]");
            }
        }
    }
    std::printf("stair math 137C0/13C80/12910 sanity OK\n");
    return true;
}

int RunAllTests() {
    int failed_scenarios = 0;

    {
        TestHost host;
        if (!verify_setup()) ++failed_scenarios;
    }
    {
        TestHost host;
        if (!verify_ui()) ++failed_scenarios;
    }
    if (!verify_edges()) ++failed_scenarios;
    if (!verify_stair_math()) ++failed_scenarios;

    for (int depth = 8; depth <= 32; depth <<= 1) {
        {
            TestHost host;
            if (!ScenarioPassthrough(host, depth)) ++failed_scenarios;
        }
        {
            TestHost host;
            if (!ScenarioV2Roundtrip(host, depth)) ++failed_scenarios;
        }
        {
            TestHost host;
            if (!ScenarioKey(host, depth, false)) ++failed_scenarios;
        }
        {
            TestHost host;
            if (!ScenarioKey(host, depth, true)) ++failed_scenarios;
        }
        {
            TestHost host;
            if (!ScenarioFamilyA(host, depth)) ++failed_scenarios;
        }
        {
            TestHost host;
            if (!ScenarioCorner(host, depth)) ++failed_scenarios;
        }
        {
            TestHost host;
            if (!ScenarioGamma(host, depth)) ++failed_scenarios;
        }
    }

    std::printf("\n%d checks, %d failures, %d failed scenarios\n", g_checks,
                g_failures, failed_scenarios);
    return (g_failures == 0) ? 0 : 1;
}

}  // namespace

int main() {
    return RunAllTests();
}

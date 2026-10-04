// Fake AE host; production EffectMain runs PARAMS_SETUP -> SMART_PRE_RENDER ->
// SMART_RENDER below. Covers: 224-param identities/defaults, UPDATE_PARAMS_UI
// visibility for all control modes via mocked AEGP suites, exact key/keep
// alpha + RGB preservation, replacement gating and first-enabled-hit, all six
// color spaces, per-color/per-component/both thresholds, premultiplied
// matching, epsilon/precision boundaries, thin erosion/growth with source
// restore, box/approx/Euclidean distances (incl. non-square downsample and
// no-seed), Inside/Around/Outside blur profiles, Color Keep inversion,
// padded row stride, sub-extent renders, count>25 rejection and callback
// balance, at 8/16/32 bpc.
#include "OLMColorKey.cpp"

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
    bool pad_new_worlds = false;

    std::array<PF_ParamDef, OLMCK_NUM_PARAMS> params{};
    std::array<PF_ParamDef, OLMCK_NUM_PARAMS> added{};
    int add_count = 0;
    int param_checkouts = 0;
    int param_checkins = 0;
    int layer_checkouts = 0;
    int layer_checkins = 0;
    int output_checkouts = 0;

    PF_ColorParamSuite1 color_suite{};
    PF_ParamUtilsSuite3 param_utils_suite{};
    PF_Iterate8Suite1 iterate8{};
    PF_Iterate16Suite1 iterate16{};
    PF_IterateFloatSuite1 iteratef{};
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

    // Fault injection: fail AcquireSuite for this suite name; when
    // permissive_checkout is set, out-of-range positions are served with a
    // zeroed definition instead of an error and the maximum requested
    // position is recorded.
    const char *fail_suite = nullptr;
    bool permissive_checkout = false;
    A_long max_checkout_position = 0;

    A_long PixelSize() const { return bitdepth == 8 ? 4 : bitdepth == 16 ? 8 : 16; }
};

TestHost *g_host = nullptr;

template <typename Pixel>
Pixel *WorldPixel(PF_EffectWorld &world, A_long x, A_long y) {
    return (Pixel *)((char *)world.data + (size_t)y * (size_t)world.rowbytes +
                     (size_t)x * sizeof(Pixel));
}

A_u_long Encode(TestHost &host, float value) {
    if (host.bitdepth == 8) return (A_u_long)(A_u_char)(int)(value * 255.0f);
    if (host.bitdepth == 16) return (A_u_long)(A_u_short)(int)(value * 32768.0f);
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
             float b, float a) {
    const PixelValues v = ReadPixel(host, world, x, y);
    if (host.bitdepth == 32) {
        return fabsf(v.r - r) <= 1e-6f && fabsf(v.g - g) <= 1e-6f &&
               fabsf(v.b - b) <= 1e-6f && fabsf(v.a - a) <= 1e-6f;
    }
    const float tol = host.bitdepth == 8 ? 0.5f / 255.0f : 0.5f / 32768.0f;
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
    if (!g_host || !def || g_host->add_count >= OLMCK_NUM_PARAMS - 1)
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    ++g_host->add_count;
    g_host->added[(size_t)g_host->add_count] = *def;
    g_host->params[(size_t)g_host->add_count] = *def;
    return PF_Err_NONE;
}

PF_Err checkout_param(PF_ProgPtr, PF_ParamIndex position, A_long, A_long, A_u_long,
                      PF_ParamDef *param) {
    if (!g_host || !param || position <= OLMCK_INPUT) return PF_Err_BAD_CALLBACK_PARAM;
    if (position >= OLMCK_NUM_PARAMS && !g_host->permissive_checkout)
        return PF_Err_BAD_CALLBACK_PARAM;
    if (position > g_host->max_checkout_position)
        g_host->max_checkout_position = position;
    ++g_host->param_checkouts;
    if (position >= OLMCK_NUM_PARAMS) {
        AEFX_CLR_STRUCT(*param);  // permissive host serves a blank definition
        return PF_Err_NONE;
    }
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

// PF_SPRINTF goes through in_data->utils->ansi.sprintf. The effect only
// formats short names, so a bounded buffer keeps msvcrt's int-count
// vsnprintf happy.
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
    g_host->ui_records.push_back({position, *def});
    return PF_Err_NONE;
}

template <typename Pixel>
PF_Err IterateWorlds(PF_EffectWorld *src, PF_EffectWorld *dst, const PF_Rect *area,
                     PF_Err (*fn)(void *, A_long, A_long, Pixel *, Pixel *), void *refcon) {
    if (!src || !dst || !fn || !src->data || !dst->data) return PF_Err_BAD_CALLBACK_PARAM;
    A_long x0 = 0, y0 = 0, x1 = dst->width, y1 = dst->height;
    if (area) {
        x0 = area->left;
        y0 = area->top;
        x1 = area->right;
        y1 = area->bottom;
    }
    for (A_long y = y0; y < y1; ++y) {
        for (A_long x = x0; x < x1; ++x) {
            PF_Err err = fn(refcon, x, y, WorldPixel<Pixel>(*src, x, y),
                            WorldPixel<Pixel>(*dst, x, y));
            if (err) return err;
        }
    }
    return PF_Err_NONE;
}

PF_Err iterate8(PF_InData *, A_long, A_long, PF_EffectWorld *src, const PF_Rect *area,
                void *refcon, PF_IteratePixel8Func fn, PF_EffectWorld *dst) {
    return IterateWorlds<PF_Pixel>(src, dst, area, fn, refcon);
}

PF_Err iterate16(PF_InData *, A_long, A_long, PF_EffectWorld *src, const PF_Rect *area,
                 void *refcon, PF_IteratePixel16Func fn, PF_EffectWorld *dst) {
    return IterateWorlds<PF_Pixel16>(src, dst, area, fn, refcon);
}

PF_Err iteratef(PF_InData *, A_long, A_long, PF_EffectWorld *src, const PF_Rect *area,
                void *refcon, PF_IteratePixelFloatFunc fn, PF_EffectWorld *dst) {
    return IterateWorlds<PF_PixelFloat>(src, dst, area, fn, refcon);
}

PF_Err new_world(PF_ProgPtr, A_long width, A_long height, PF_Boolean clear,
                 PF_PixelFormat format, PF_EffectWorld *world) {
    if (!g_host || !world || width < 0 || height < 0) return PF_Err_BAD_CALLBACK_PARAM;
    const A_long px = format == PF_PixelFormat_ARGB32 ? 4
                      : format == PF_PixelFormat_ARGB64 ? 8
                                                        : 16;
    WorldBuffer *buf = new WorldBuffer();
    buf->rowbytes = width * px + (g_host->pad_new_worlds ? 64 : 0);
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
    if (g_host->fail_suite && !std::strcmp(name, g_host->fail_suite)) return (SPErr)-1;
    if (!std::strcmp(name, kPFColorParamSuite) && version == kPFColorParamSuiteVersion1)
        *suite = &g_host->color_suite;
    else if (!std::strcmp(name, kPFParamUtilsSuite) && version == kPFParamUtilsSuiteVersion3)
        *suite = &g_host->param_utils_suite;
    else if (!std::strcmp(name, kPFIterate8Suite) && version == kPFIterate8SuiteVersion1)
        *suite = &g_host->iterate8;
    else if (!std::strcmp(name, kPFIterate16Suite) && version == kPFIterate16SuiteVersion1)
        *suite = &g_host->iterate16;
    else if (!std::strcmp(name, kPFIterateFloatSuite) &&
             version == kPFIterateFloatSuiteVersion1)
        *suite = &g_host->iteratef;
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
    if (!g_host || !result || position != OLMCK_INPUT || checkout_id != 0)
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

bool init_host(TestHost &host, int bitdepth, bool pad_rows = false, A_long width = kWidth,
               A_long height = kHeight) {
    g_host = &host;
    host.bitdepth = bitdepth;
    host.width = width;
    host.height = height;
    host.max_checkout_position = 0;
    host.pixel_size = host.PixelSize();
    host.rowbytes = width * host.pixel_size + (pad_rows ? 64 : 0);
    host.input_bytes.assign((size_t)host.rowbytes * (size_t)height, 0);
    host.output_bytes.assign((size_t)host.rowbytes * (size_t)height, 0);
    set_world(host.input, host.input_bytes, host.rowbytes, width, height);
    set_world(host.output, host.output_bytes, host.rowbytes, width, height);

    host.color_suite.PF_GetFloatingPointColorFromColorDef = get_color;
    host.param_utils_suite.PF_UpdateParamUI = update_param_ui;
    host.iterate8.iterate = iterate8;
    host.iterate16.iterate = iterate16;
    host.iteratef.iterate = iteratef;
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
        host.out.num_params != OLMCK_NUM_PARAMS || host.add_count != OLMCK_NUM_PARAMS - 1)
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

// Restores every parameter to its ParamsSetup default (scenarios share one
// host; without this, tweaks leak between scenarios).
void ResetParams(TestHost &host) {
    for (int i = 1; i < OLMCK_NUM_PARAMS; ++i)
        host.params[(size_t)i] = host.added[(size_t)i];
}

// The value a truncated 8/16-bit channel decodes back to.
float EncDec(TestHost &host, float v) {
    return Decode(host, Encode(host, v));
}

// The value the Color-Keep-OFF inversion (srcA - keyedA in channel units)
// decodes back to.
float InvDec(TestHost &host, float src, float keyed) {
    if (host.bitdepth == 32) return src - keyed;
    return Decode(host, Encode(host, src) - Encode(host, keyed));
}

// Parameter tweak helpers (positions, not ids).
void SetKeep(TestHost &h, bool on) { h.params[OLMCK_COLOR_KEEP].u.bd.value = on ? 1 : 0; }
void SetPremul(TestHost &h, bool on) {
    h.params[OLMCK_PREMULTIPLIED].u.bd.value = on ? 1 : 0;
}
void SetSpace(TestHost &h, A_long v) { h.params[OLMCK_COLOR_SPACE].u.pd.value = v; }
void SetPerColor(TestHost &h, bool on) {
    h.params[OLMCK_PER_COLOR].u.bd.value = on ? 1 : 0;
}
void SetPerComponent(TestHost &h, bool on) {
    h.params[OLMCK_PER_COMPONENT].u.bd.value = on ? 1 : 0;
}
void SetCount(TestHost &h, A_long v) { h.params[OLMCK_COLOR_COUNT].u.sd.value = v; }
void SetEnableReplace(TestHost &h, bool on) {
    h.params[OLMCK_ENABLE_REPLACE].u.bd.value = on ? 1 : 0;
}
void SetGlobalThr(TestHost &h, float v) { h.params[OLMCK_THRESHOLD].u.fs_d.value = v; }
void SetThin(TestHost &h, A_long amount, A_long dist) {
    h.params[OLMCK_THIN_AMOUNT].u.sd.value = amount;
    h.params[OLMCK_THIN_DISTANCE].u.pd.value = dist;
}
void SetBlur(TestHost &h, float amount, A_long dist, A_long dir) {
    h.params[OLMCK_BLUR_AMOUNT].u.fs_d.value = amount;
    h.params[OLMCK_BLUR_DISTANCE].u.pd.value = dist;
    h.params[OLMCK_BLUR_DIRECTION].u.pd.value = dir;
}
void SetKeyColor(TestHost &h, int slot, float r, float g, float b) {
    PF_ParamDef &def = h.params[(size_t)(OLMCK_SLOT_BASE + slot * 8 + 2)];
    def.u.cd.value.red = (A_u_char)std::min(255.0f, std::max(0.0f, r * 255.0f));
    def.u.cd.value.green = (A_u_char)std::min(255.0f, std::max(0.0f, g * 255.0f));
    def.u.cd.value.blue = (A_u_char)std::min(255.0f, std::max(0.0f, b * 255.0f));
    def.u.cd.value.alpha = 255;
}
void SetReplaceColor(TestHost &h, int slot, float r, float g, float b) {
    PF_ParamDef &def = h.params[(size_t)(OLMCK_SLOT_BASE + slot * 8 + 3)];
    def.u.cd.value.red = (A_u_char)std::min(255.0f, std::max(0.0f, r * 255.0f));
    def.u.cd.value.green = (A_u_char)std::min(255.0f, std::max(0.0f, g * 255.0f));
    def.u.cd.value.blue = (A_u_char)std::min(255.0f, std::max(0.0f, b * 255.0f));
    def.u.cd.value.alpha = 255;
}
void SetUseColor(TestHost &h, int slot, bool on) {
    h.params[(size_t)(OLMCK_SLOT_BASE + slot * 8 + 0)].u.bd.value = on ? 1 : 0;
}
void SetUseReplace(TestHost &h, int slot, bool on) {
    h.params[(size_t)(OLMCK_SLOT_BASE + slot * 8 + 1)].u.bd.value = on ? 1 : 0;
}
void SetSlotThr(TestHost &h, int slot, float t) {
    h.params[(size_t)(OLMCK_SLOT_BASE + slot * 8 + 4)].u.fs_d.value = t;
}
void SetSlotThrRGB(TestHost &h, int slot, float tr, float tg, float tb) {
    h.params[(size_t)(OLMCK_SLOT_BASE + slot * 8 + 5)].u.fs_d.value = tr;
    h.params[(size_t)(OLMCK_SLOT_BASE + slot * 8 + 6)].u.fs_d.value = tg;
    h.params[(size_t)(OLMCK_SLOT_BASE + slot * 8 + 7)].u.fs_d.value = tb;
}
void SetGlobalThrRGB(TestHost &h, float tr, float tg, float tb) {
    h.params[OLMCK_THRESHOLD_R].u.fs_d.value = tr;
    h.params[OLMCK_THRESHOLD_G].u.fs_d.value = tg;
    h.params[OLMCK_THRESHOLD_B].u.fs_d.value = tb;
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

bool CheckFloatSlider(const PF_ParamDef &def, const char *name, double vmin, double vmax,
                      double smin, double smax, double dflt, int prec, bool collapse,
                      A_long id) {
    if (def.uu.id == id && def.param_type == PF_Param_FLOAT_SLIDER &&
        SameString(def.PF_DEF_NAME, name) && def.u.fs_d.valid_min == vmin &&
        def.u.fs_d.valid_max == vmax && def.u.fs_d.slider_min == smin &&
        def.u.fs_d.slider_max == smax && def.u.fs_d.dephault == dflt &&
        def.u.fs_d.precision == prec &&
        (def.flags & PF_ParamFlag_COLLAPSE_TWIRLY) ==
            (collapse ? (PF_ParamFlags)PF_ParamFlag_COLLAPSE_TWIRLY : (PF_ParamFlags)0))
        return true;
    std::printf("  diag: id=%d want=%d type=%d name='%s' want='%s' vmin=%g vmax=%g "
                "smin=%g smax=%g dflt=%g prec=%d flags=%lx\n",
                (int)def.uu.id, (int)id, (int)def.param_type, def.PF_DEF_NAME, name,
                (double)def.u.fs_d.valid_min, (double)def.u.fs_d.valid_max,
                (double)def.u.fs_d.slider_min, (double)def.u.fs_d.slider_max,
                (double)def.u.fs_d.dephault, (int)def.u.fs_d.precision,
                (unsigned long)def.flags);
    return false;
}

bool verify_setup() {
    TestHost host;
    if (!init_host(host, 8)) {
        std::printf("FAIL: host init / params setup\n");
        ++g_failures;
        return false;
    }
    CHECK(host.out.num_params == 224, "num_params == 224");
    CHECK(host.add_count == 223, "223 params added");

    const PF_ParamDef &keep = host.added[1];
    CHECK(keep.uu.id == 1 && keep.param_type == PF_Param_CHECKBOX &&
              SameString(keep.PF_DEF_NAME, "Color Keep") && keep.u.bd.dephault == 0,
          "pos1 Color Keep checkbox");
    CHECK(CheckFloatSlider(host.added[2], "Threshold", 0.0, 1.0, 0.0, 1.0, 0.0, 4, true, 2),
          "pos2 Threshold float slider");
    CHECK(host.added[3].param_type == PF_Param_GROUP_START &&
              SameString(host.added[3].PF_DEF_NAME, "Threshold Parameters") &&
              host.added[3].uu.id == 3,
          "pos3 Threshold Parameters topic");
    CHECK(host.added[4].uu.id == 4 && host.added[4].param_type == PF_Param_CHECKBOX &&
              SameString(host.added[4].PF_DEF_NAME, "Premultiplied Color") &&
              host.added[4].u.bd.dephault == 0,
          "pos4 Premultiplied Color");
    {
        const PF_ParamDef &def = host.added[5];
        CHECK(def.uu.id == 5 && def.param_type == PF_Param_POPUP &&
                  def.u.pd.num_choices == 6 && def.u.pd.dephault == 1 &&
                  SameString(def.PF_DEF_NAME, "Color Space") &&
                  SameString(def.u.pd.u.PF_DEF_NAMESPTR, "RGB|HSV|Lab76|Lab94|YUV|YCrCb"),
              "pos5 Color Space popup");
    }
    {
        const PF_ParamDef &def = host.added[6];
        CHECK(def.uu.id == 522 && def.param_type == PF_Param_POPUP &&
                  def.u.pd.num_choices == 3 && def.u.pd.dephault == 1 &&
                  SameString(def.PF_DEF_NAME, "Force Lower Precision") &&
                  SameString(def.u.pd.u.PF_DEF_NAMESPTR, "Full|16bit|8bit"),
              "pos6 Force Lower Precision popup (id 522 at position 6)");
    }
    CHECK(host.added[7].uu.id == 6 && host.added[7].u.bd.dephault == 0 &&
              SameString(host.added[7].PF_DEF_NAME, "Per Color"),
          "pos7 Per Color");
    CHECK(host.added[8].uu.id == 7 && host.added[8].u.bd.dephault == 0 &&
              SameString(host.added[8].PF_DEF_NAME, "Per Component"),
          "pos8 Per Component");
    CHECK(CheckFloatSlider(host.added[9], "Threshold(R,H,L,Y,Y)", 0.0, 1.0, 0.0, 1.0, 0.0,
                           4, true, 8),
          "pos9 Threshold(R,H,L,Y,Y)");
    CHECK(CheckFloatSlider(host.added[10], "Threshold(G,S,a,U,Cr)", 0.0, 1.0, 0.0, 1.0,
                           0.0, 4, true, 9),
          "pos10 Threshold(G,S,a,U,Cr)");
    CHECK(CheckFloatSlider(host.added[11], "Threshold(B,V,b,V,Cb)", 0.0, 1.0, 0.0, 1.0,
                           0.0, 4, true, 10),
          "pos11 Threshold(B,V,b,V,Cb)");
    CHECK(host.added[12].param_type == PF_Param_GROUP_END && host.added[12].uu.id == 11,
          "pos12 threshold group end");
    CHECK(host.added[13].param_type == PF_Param_GROUP_START &&
              SameString(host.added[13].PF_DEF_NAME, "Edge Thin") &&
              host.added[13].uu.id == 12,
          "pos13 Edge Thin topic");
    {
        const PF_ParamDef &def = host.added[14];
        CHECK(def.uu.id == 13 && def.param_type == PF_Param_SLIDER &&
                  SameString(def.PF_DEF_NAME, "Amount") &&
                  def.u.sd.valid_min == -4000 && def.u.sd.valid_max == 4000 &&
                  def.u.sd.slider_min == -100 && def.u.sd.slider_max == 100 &&
                  def.u.sd.dephault == 0,
              "pos14 Edge Thin Amount slider (valid -4000..4000, slider -100..100)");
    }
    {
        const PF_ParamDef &def = host.added[15];
        CHECK(def.uu.id == 14 && def.param_type == PF_Param_POPUP &&
                  def.u.pd.num_choices == 3 && def.u.pd.dephault == 1 &&
                  SameString(def.u.pd.u.PF_DEF_NAMESPTR, "Box|Approximate|Euclidean"),
              "pos15 thin Distance Type popup");
    }
    CHECK(host.added[16].param_type == PF_Param_GROUP_END && host.added[16].uu.id == 20,
          "pos16 thin group end (id 20)");
    CHECK(host.added[17].param_type == PF_Param_GROUP_START &&
              SameString(host.added[17].PF_DEF_NAME, "Edge Blur") &&
              host.added[17].uu.id == 16,
          "pos17 Edge Blur topic (id 16)");
    CHECK(CheckFloatSlider(host.added[18], "Amount", 0.0, 4000.0, 0.0, 100.0, 0.0, 1,
                           false, 17),
          "pos18 Edge Blur Amount (valid 0..4000, slider 0..100, prec 1)");
    CHECK(host.added[19].uu.id == 18 && host.added[19].u.pd.num_choices == 3 &&
              host.added[19].u.pd.dephault == 1,
          "pos19 blur Distance Type");
    {
        const PF_ParamDef &def = host.added[20];
        CHECK(def.uu.id == 19 && def.u.pd.num_choices == 3 && def.u.pd.dephault == 2 &&
                  SameString(def.PF_DEF_NAME, "Direction") &&
                  SameString(def.u.pd.u.PF_DEF_NAMESPTR, "Inside | Around | Outside"),
              "pos20 Direction popup default Around");
    }
    CHECK(host.added[21].param_type == PF_Param_GROUP_END && host.added[21].uu.id == 16,
          "pos21 blur group end duplicates id 16 (binary-faithful)");
    {
        const PF_ParamDef &def = host.added[22];
        CHECK(def.uu.id == 21 && def.param_type == PF_Param_SLIDER &&
                  SameString(def.PF_DEF_NAME, "Number of Colors") &&
                  def.u.sd.valid_min == 0 && def.u.sd.valid_max == 25 &&
                  def.u.sd.slider_min == 0 && def.u.sd.slider_max == 30 &&
                  def.u.sd.dephault == 1 && (def.flags & PF_ParamFlag_SUPERVISE),
              "pos22 Number of Colors (valid 0..25, slider 0..30, SUPERVISE)");
    }
    {
        const PF_ParamDef &def = host.added[23];
        CHECK(def.uu.id == 523 && def.param_type == PF_Param_CHECKBOX &&
                  SameString(def.PF_DEF_NAME, "Enable Replace") &&
                  def.u.bd.dephault == 0 && (def.flags & PF_ParamFlag_SUPERVISE),
              "pos23 Enable Replace (SUPERVISE)");
    }
    for (int n = 0; n < OLMCK_MAX_COLORS; ++n) {
        const int base = OLMCK_SLOT_BASE + n * 8;
        char msg[128];
        char name[64];
        const PF_ParamDef &use_color = host.added[(size_t)base];
        std::snprintf(msg, sizeof(msg), "slot %d Use Color", n);
        CHECK(use_color.uu.id == 524 + 3 * n &&
                  use_color.param_type == PF_Param_CHECKBOX &&
                  use_color.u.bd.dephault == 1,
              msg);
        const PF_ParamDef &use_replace = host.added[(size_t)(base + 1)];
        std::snprintf(msg, sizeof(msg), "slot %d Use Replace Color", n);
        CHECK(use_replace.uu.id == 525 + 3 * n &&
                  use_replace.param_type == PF_Param_CHECKBOX &&
                  use_replace.u.bd.dephault == 0,
              msg);
        const PF_ParamDef &color = host.added[(size_t)(base + 2)];
        std::snprintf(msg, sizeof(msg), "slot %d Color", n);
        CHECK(color.uu.id == 22 + 5 * n && color.param_type == PF_Param_COLOR &&
                  color.u.cd.value.red == 0 && color.u.cd.value.green == 0 &&
                  color.u.cd.value.blue == 0 && color.u.cd.value.alpha == 255 &&
                  color.u.cd.dephault.alpha == 255 &&
                  (color.flags & PF_ParamFlag_COLLAPSE_TWIRLY),
              msg);
        const PF_ParamDef &replace = host.added[(size_t)(base + 3)];
        std::snprintf(msg, sizeof(msg), "slot %d Replace Color", n);
        CHECK(replace.uu.id == 526 + 3 * n && replace.param_type == PF_Param_COLOR &&
                  replace.u.cd.value.alpha == 255 &&
                  (replace.flags & PF_ParamFlag_COLLAPSE_TWIRLY),
              msg);
        std::snprintf(name, sizeof(name), "Threshold %d", n + 1);
        std::snprintf(msg, sizeof(msg), "slot %d Threshold", n);
        CHECK(CheckFloatSlider(host.added[(size_t)(base + 4)], name, 0.0, 1.0, 0.0, 1.0,
                               0.0, 4, true, 23 + 5 * n),
              msg);
        std::snprintf(name, sizeof(name), "Threshold(R,H,L,Y,Y) %d", n + 1);
        std::snprintf(msg, sizeof(msg), "slot %d Threshold R", n);
        CHECK(CheckFloatSlider(host.added[(size_t)(base + 5)], name, 0.0, 1.0, 0.0, 1.0,
                               0.0, 4, true, 24 + 5 * n),
              msg);
        std::snprintf(name, sizeof(name), "Threshold(G,S,a,U,Cr) %d", n + 1);
        std::snprintf(msg, sizeof(msg), "slot %d Threshold G", n);
        CHECK(CheckFloatSlider(host.added[(size_t)(base + 6)], name, 0.0, 1.0, 0.0, 1.0,
                               0.0, 4, true, 25 + 5 * n),
              msg);
        std::snprintf(name, sizeof(name), "Threshold(B,V,b,V,Cb) %d", n + 1);
        std::snprintf(msg, sizeof(msg), "slot %d Threshold B", n);
        CHECK(CheckFloatSlider(host.added[(size_t)(base + 7)], name, 0.0, 1.0, 0.0, 1.0,
                               0.0, 4, true, 26 + 5 * n),
              msg);
    }
    CHECK(host.added[216].uu.id == 596 && host.added[217].uu.id == 597 &&
              host.added[218].uu.id == 142 && host.added[219].uu.id == 598 &&
              host.added[220].uu.id == 143 && host.added[221].uu.id == 144 &&
              host.added[222].uu.id == 145 && host.added[223].uu.id == 146,
          "last slot ids 596/597/142/598/143/144/145/146");
    CHECK(host.added[9].uu.id == 8 && host.added[8].uu.id == 7,
          "position index differs from uu.id");

    CHECK(host.out.my_version == 1148928, "my_version == PF_VERSION(2,3,1,DEVELOP,0)");
    CHECK(host.out.out_flags == 0x06000440,
          "out_flags 0x06000440 (binary + SEND_UPDATE_PARAMS_UI deviation)");
    CHECK(host.out.out_flags2 == 0x08001400, "out_flags2 0x08001400");
    CHECK(host.registered_id == 0x1234, "AEGP_RegisterWithAEGP called");

    {
        PF_OutData about_out;
        AEFX_CLR_STRUCT(about_out);
        EffectMain(PF_Cmd_ABOUT, &host.in, &about_out, nullptr, nullptr, nullptr);
        CHECK(SameString(about_out.return_msg,
                         "OLM Color Key 2.3.1\rKey or Keep the selected color from the "
                         "source.\rCopyright 2014 OLM Digital, Inc."),
              "About text");
    }

    teardown_host(host);
    std::printf("setup: 224 params, identities, flags, version, about OK\n");
    return true;
}

// --- 2. UPDATE_PARAMS_UI -----------------------------------------------------

struct UiExpect {
    std::map<PF_ParamIndex, bool> ui_disabled;    // PF_UpdateParamUI records
    std::map<PF_ParamIndex, bool> stream_hidden;  // SetDynamicStreamFlag records
};

void BuildUiExpect(UiExpect &e, bool per_color, bool per_component, A_long count,
                   bool enable_replace, const bool *use_color, const bool *use_replace) {
    e.ui_disabled.clear();
    e.stream_hidden.clear();
    e.ui_disabled[OLMCK_THRESHOLD] = per_color || per_component;
    const bool component_visible = per_component && !per_color;
    e.stream_hidden[OLMCK_THRESHOLD_R] = !component_visible;
    e.stream_hidden[OLMCK_THRESHOLD_G] = !component_visible;
    e.stream_hidden[OLMCK_THRESHOLD_B] = !component_visible;
    for (int n = 0; n < OLMCK_MAX_COLORS; ++n) {
        const int base = OLMCK_SLOT_BASE + n * 8;
        const bool active = n < count;
        const bool replace_visible = active && enable_replace;
        const bool replace_pickable = replace_visible && use_replace[n];
        const bool use_color_off = !use_color[n];
        e.stream_hidden[base + 2] = !active;            // Color n
        e.stream_hidden[base + 0] = !active;            // Use Color n
        e.stream_hidden[base + 1] = !replace_visible;   // Use Replace n
        e.stream_hidden[base + 3] = !replace_pickable;  // Replace Color n
        e.ui_disabled[base + 2] = use_color_off;
        e.ui_disabled[base + 1] = use_color_off;
        e.ui_disabled[base + 3] = use_color_off;
        e.stream_hidden[base + 4] = !(active && per_color && !per_component);
        e.stream_hidden[base + 5] = !(active && per_color && per_component);
        e.stream_hidden[base + 6] = !(active && per_color && per_component);
        e.stream_hidden[base + 7] = !(active && per_color && per_component);
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

bool RunUiMode(TestHost &host, bool per_color, bool per_component, A_long count,
               bool enable_replace, const bool *use_color, const bool *use_replace,
               const char *label) {
    SetPerColor(host, per_color);
    SetPerComponent(host, per_component);
    SetCount(host, count);
    SetEnableReplace(host, enable_replace);
    for (int n = 0; n < OLMCK_MAX_COLORS; ++n) {
        SetUseColor(host, n, use_color[n]);
        SetUseReplace(host, n, use_replace[n]);
    }
    host.ui_records.clear();
    host.stream_records.clear();
    host.effect_disposes = 0;
    host.stream_disposes = 0;
    std::vector<PF_ParamDef *> params((size_t)OLMCK_NUM_PARAMS);
    for (int i = 0; i < OLMCK_NUM_PARAMS; ++i) params[(size_t)i] = &host.params[(size_t)i];
    const PF_Err err = EffectMain(PF_Cmd_UPDATE_PARAMS_UI, &host.in, &host.out,
                                  params.data(), nullptr, nullptr);
    if (err != PF_Err_NONE) {
        std::printf("FAIL: UPDATE_PARAMS_UI errored in %s (%d)\n", label, (int)err);
        ++g_failures;
        return false;
    }
    UiExpect e;
    BuildUiExpect(e, per_color, per_component, count, enable_replace, use_color,
                  use_replace);
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
    bool use_color[OLMCK_MAX_COLORS];
    bool use_replace[OLMCK_MAX_COLORS];
    for (int n = 0; n < OLMCK_MAX_COLORS; ++n) {
        use_color[n] = true;
        use_replace[n] = false;
    }
    use_replace[0] = true;
    if (!RunUiMode(host, false, false, 2, true, use_color, use_replace, "neither"))
        return false;
    if (!RunUiMode(host, true, false, 2, true, use_color, use_replace, "perColor"))
        return false;
    if (!RunUiMode(host, false, true, 2, true, use_color, use_replace, "perComponent"))
        return false;
    if (!RunUiMode(host, true, true, 2, true, use_color, use_replace, "both")) return false;
    for (int n = 0; n < OLMCK_MAX_COLORS; ++n) {
        use_color[n] = true;
        use_replace[n] = false;
    }
    use_color[0] = false;
    use_replace[0] = true;
    if (!RunUiMode(host, true, true, 2, false, use_color, use_replace,
                   "both/useColor0-off/replace-off"))
        return false;
    for (int n = 0; n < OLMCK_MAX_COLORS; ++n) {
        use_color[n] = true;
        use_replace[n] = false;
    }
    if (!RunUiMode(host, false, false, 0, true, use_color, use_replace, "count0"))
        return false;
    teardown_host(host);
    return true;
}

// --- 3. render scenarios -----------------------------------------------------

bool ScenarioKeyOutBlack(TestHost &host, const char *label) {
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 1, 1, 1, 1);
    FillRect(host, 2, 2, 6, 4, 0, 0, 0, 1);
    if (!RenderChecked(host, label)) return false;
    CHECK(PixelIs(host, host.output, 4, 3, 0, 0, 0, 0), "keyed black alpha keyed out");
    CHECK(PixelIs(host, host.output, 0, 0, 1, 1, 1, 1), "white pixel untouched");
    CHECK(PixelIs(host, host.output, 2, 2, 0, 0, 0, 0), "rect corner keyed out");
    std::printf("%s OK\n", label);
    return true;
}

bool ScenarioKeep(TestHost &host, const char *label) {
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 1, 1, 1, 1);
    FillRect(host, 2, 2, 6, 4, 0, 0, 0, 1);
    SetKeep(host, true);
    if (!RenderChecked(host, label)) return false;
    CHECK(PixelIs(host, host.output, 4, 3, 0, 0, 0, 1), "keyed black kept (alpha=srcA)");
    CHECK(PixelIs(host, host.output, 0, 0, 1, 1, 1, 0), "white removed (alpha 0)");
    CHECK(PixelIs(host, host.output, 2, 2, 0, 0, 0, 1), "rect corner kept");
    std::printf("%s OK\n", label);
    return true;
}

bool ScenarioPartialAlphaAndRgb(TestHost &host, const char *label) {
    SetKeep(host, true);
    SetKeyColor(host, 0, 1, 0, 0);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 0.25f, 0.5f, 0.75f, 0.5f);
    FillRect(host, 1, 1, 3, 3, 1, 0, 0, 1);
    FillRect(host, 5, 4, 7, 5, 1, 0, 0, 0.25f);
    if (!RenderChecked(host, label)) return false;
    CHECK(PixelIs(host, host.output, 2, 2, 1, 0, 0, 1), "opaque hit keeps alpha");
    CHECK(PixelIs(host, host.output, 6, 4, 1, 0, 0, EncDec(host, 0.25f)),
          "partial-alpha hit keeps alpha");
    CHECK(PixelIs(host, host.output, 0, 0, EncDec(host, 0.25f), EncDec(host, 0.5f),
                  EncDec(host, 0.75f), 0),
          "miss loses alpha, RGB preserved");
    CHECK(PixelIs(host, host.output, 4, 3, EncDec(host, 0.25f), EncDec(host, 0.5f),
                  EncDec(host, 0.75f), 0),
          "miss RGB preserved 2");
    std::printf("%s OK\n", label);
    return true;
}

bool ScenarioReplace(TestHost &host, const char *label) {
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 1, 1, 1, 1);
    FillRect(host, 2, 2, 6, 4, 0, 0, 0, 1);
    SetKeep(host, true);
    SetEnableReplace(host, true);
    SetUseReplace(host, 0, true);
    SetReplaceColor(host, 0, 1, 0, 0);
    if (!RenderChecked(host, label)) return false;
    CHECK(PixelIs(host, host.output, 4, 3, 1, 0, 0, 1), "hit pixel replaced with red");
    CHECK(PixelIs(host, host.output, 0, 0, 1, 1, 1, 0), "miss untouched");
    std::printf("%s OK\n", label);
    return true;
}

bool ScenarioReplaceGates(TestHost &host) {
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 1, 1, 1, 1);
    FillRect(host, 2, 2, 6, 4, 0, 0, 0, 1);
    SetKeep(host, true);
    SetEnableReplace(host, true);
    SetReplaceColor(host, 0, 1, 0, 0);
    SetUseReplace(host, 0, false);
    if (!RenderChecked(host, "replace gated by Use Replace Color 1")) return false;
    CHECK(PixelIs(host, host.output, 4, 3, 0, 0, 0, 1), "Use Replace off -> no replace");

    SetUseReplace(host, 0, true);
    SetEnableReplace(host, false);
    if (!RenderChecked(host, "replace gated by Enable Replace")) return false;
    CHECK(PixelIs(host, host.output, 4, 3, 0, 0, 0, 1), "Enable Replace off -> no replace");

    SetEnableReplace(host, true);
    SetKeep(host, false);
    if (!RenderChecked(host, "replace gated by Color Keep")) return false;
    CHECK(PixelIs(host, host.output, 4, 3, 0, 0, 0, 0), "keep off -> no replace");
    std::printf("replace gates OK\n");
    return true;
}

bool ScenarioFirstHit(TestHost &host, const char *label) {
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 1, 1, 1, 1);
    FillRect(host, 2, 2, 6, 4, 0, 0, 0, 1);
    SetKeep(host, true);
    SetEnableReplace(host, true);
    SetCount(host, 2);
    SetKeyColor(host, 0, 0, 0, 0);
    SetKeyColor(host, 1, 0, 0, 0);
    SetReplaceColor(host, 0, 1, 0, 0);
    SetReplaceColor(host, 1, 0, 1, 0);
    SetUseReplace(host, 0, true);
    SetUseReplace(host, 1, true);
    if (!RenderChecked(host, label)) return false;
    CHECK(PixelIs(host, host.output, 4, 3, 1, 0, 0, 1), "first enabled hit wins (red)");
    SetUseColor(host, 0, false);
    if (!RenderChecked(host, label)) return false;
    CHECK(PixelIs(host, host.output, 4, 3, 0, 1, 0, 1),
          "disabled slot skipped, next enabled hit wins (green)");
    std::printf("%s OK\n", label);
    return true;
}

bool ScenarioColorSpaces(TestHost &host) {
    // HSV: key pure red (h=0, s=1, v=1).
    SetKeep(host, true);
    SetSpace(host, OLMCK_SPACE_HSV);
    SetPerComponent(host, true);
    SetGlobalThrRGB(host, 0.1f, 0.1f, 0.1f);
    SetKeyColor(host, 0, 1, 0, 0);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 0, 0, 0, 1);
    FillRect(host, 1, 1, 1, 1, 1, 0, 0, 1);
    FillRect(host, 3, 1, 3, 1, 1, 0.5f, 0.5f, 1);
    FillRect(host, 5, 1, 5, 1, 1, 0, 1, 1);
    if (!RenderChecked(host, "HSV component")) return false;
    CHECK(PixelIs(host, host.output, 1, 1, 1, 0, 0, 1), "HSV exact red hit");
    CHECK(PixelIs(host, host.output, 3, 1, 1, 0.5f, 0.5f, 0), "HSV saturation delta miss");
    CHECK(PixelIs(host, host.output, 5, 1, 1, 0, 1, 0), "HSV hue delta miss");
    SetPerComponent(host, false);
    SetGlobalThr(host, 0.4f);
    if (!RenderChecked(host, "HSV aggregate")) return false;
    CHECK(PixelIs(host, host.output, 3, 1, 1, 0.5f, 0.5f, 1), "HSV aggregate S-delta hit");
    CHECK(PixelIs(host, host.output, 5, 1, 1, 0, 1, 0), "HSV aggregate hue miss");
    SetKeyColor(host, 0, 1, 0, 1);  // magenta: h = 300 deg = 0.8333
    SetPerComponent(host, true);
    SetGlobalThrRGB(host, 0.2f, 0.1f, 0.1f);
    if (!RenderChecked(host, "HSV hue wrap component")) return false;
    CHECK(PixelIs(host, host.output, 1, 1, 1, 0, 0, 1),
          "HSV component hue wrap hits (0.1667 <= 0.2)");
    SetPerComponent(host, false);
    SetGlobalThr(host, 0.2f);
    if (!RenderChecked(host, "HSV hue wrap aggregate")) return false;
    CHECK(PixelIs(host, host.output, 1, 1, 1, 0, 0, 0), "HSV aggregate does not wrap hue");

    // YUV: key white; known vectors white -> (1, 1e-5, 0), gray -> (0.5, 5e-6, 0).
    SetSpace(host, OLMCK_SPACE_YUV);
    SetKeyColor(host, 0, 1, 1, 1);
    SetPerComponent(host, true);
    SetGlobalThrRGB(host, 0.1f, 0.1f, 0.1f);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 0, 0, 0, 1);
    FillRect(host, 1, 1, 1, 1, 1, 1, 1, 1);
    FillRect(host, 3, 1, 3, 1, 0.5f, 0.5f, 0.5f, 1);
    if (!RenderChecked(host, "YUV component")) return false;
    CHECK(PixelIs(host, host.output, 1, 1, 1, 1, 1, 1), "YUV white hit");
    CHECK(PixelIs(host, host.output, 3, 1, EncDec(host, 0.5f), EncDec(host, 0.5f),
                  EncDec(host, 0.5f), 0),
          "YUV luma delta miss");
    SetGlobalThrRGB(host, 0.03f, 0.1f, 0.1f);
    SetKeyColor(host, 0, 0, 0, 0);
    FillRect(host, 1, 1, 1, 1, 0, 0, 0.183486f, 1);
    FillRect(host, 3, 1, 3, 1, 0, 0, 0.25f, 1);
    if (!RenderChecked(host, "YUV mapped-U boundary")) return false;
    CHECK(PixelIs(host, host.output, 1, 1, 0, 0, EncDec(host, 0.183486f), 1),
          "YUV mapped dU 0.0917 hit");
    CHECK(PixelIs(host, host.output, 3, 1, 0, 0, EncDec(host, 0.25f), 0),
          "YUV mapped dU 0.125 miss");

    // YCrCb: only Y and Cb compared; Cr may differ arbitrarily.
    SetSpace(host, OLMCK_SPACE_YCRCB);
    SetPerComponent(host, true);
    SetGlobalThrRGB(host, 0.02f, 0.02f, 0.02f);
    SetKeyColor(host, 0, 0.0f, 0.754828f, 0.500048f);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 0, 0, 0, 1);
    FillRect(host, 1, 1, 1, 1, 0.0f, 0.754828f, 0.500048f, 1);
    FillRect(host, 3, 1, 3, 1, 0.5f, 0.500003f, 0.500014f, 1);
    FillRect(host, 5, 1, 5, 1, 0.5f, 0.3f, 0.5f, 1);
    if (!RenderChecked(host, "YCrCb Cr ignored")) return false;
    CHECK(PixelIs(host, host.output, 1, 1, 0.0f, EncDec(host, 0.754828f),
                  EncDec(host, 0.500048f), 1),
          "YCrCb key hit");
    CHECK(PixelIs(host, host.output, 3, 1, EncDec(host, 0.5f), EncDec(host, 0.500003f),
                  EncDec(host, 0.500014f), 1),
          "YCrCb Cr difference ignored");
    CHECK(PixelIs(host, host.output, 5, 1, EncDec(host, 0.5f), EncDec(host, 0.3f),
                  EncDec(host, 0.5f), 0),
          "YCrCb Y delta miss");

    // Lab76: black key vs white; aggregate limit (eps+thr)*424.43527.
    SetSpace(host, OLMCK_SPACE_LAB76);
    SetPerComponent(host, false);
    SetKeyColor(host, 0, 0, 0, 0);
    SetGlobalThr(host, 0.1f);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 0, 0, 0, 1);
    FillRect(host, 1, 1, 1, 1, 1, 1, 1, 1);
    if (!RenderChecked(host, "Lab76 aggregate small thr")) return false;
    CHECK(PixelIs(host, host.output, 1, 1, 1, 1, 1, 0), "Lab76 white vs black misses");
    SetGlobalThr(host, 0.37f);
    if (!RenderChecked(host, "Lab76 aggregate large thr")) return false;
    CHECK(PixelIs(host, host.output, 1, 1, 1, 1, 1, 1), "Lab76 white hits at thr 0.37");

    // Lab94 component path (with the a/b shift): key white vs a 0.75 gray.
    // dL = L(0.75) - L(1) ~ 136.0 - 151.3 = -15.3; limit (eps+0.11)*151.30099
    // ~ 16.7 -> hit; with thrL 0.09 the limit ~13.7 -> miss. dA/dB are small.
    SetSpace(host, OLMCK_SPACE_LAB94);
    SetPerComponent(host, false);
    SetGlobalThr(host, 0.1f);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 0, 0, 0, 1);
    FillRect(host, 1, 1, 1, 1, 1, 1, 1, 1);
    if (!RenderChecked(host, "Lab94 aggregate")) return false;
    CHECK(PixelIs(host, host.output, 1, 1, 1, 1, 1, 0), "Lab94 white vs black misses");
    SetPerComponent(host, true);
    SetKeyColor(host, 0, 1, 1, 1);
    SetGlobalThrRGB(host, 0.11f, 0.1f, 0.1f);
    FillRect(host, 1, 1, 1, 1, 0.75f, 0.75f, 0.75f, 1);
    if (!RenderChecked(host, "Lab94 component hit")) return false;
    CHECK(PixelIs(host, host.output, 1, 1, 0.75f, 0.75f, 0.75f, 1),
          "Lab94 component: dL 15.3 within (0.11)*151.3 limit");
    SetGlobalThrRGB(host, 0.09f, 0.1f, 0.1f);
    if (!RenderChecked(host, "Lab94 component miss")) return false;
    CHECK(PixelIs(host, host.output, 1, 1, 0.75f, 0.75f, 0.75f, 0),
          "Lab94 component: dL 15.3 beyond (0.09)*151.3 limit");
    std::printf("color spaces OK\n");
    return true;
}

bool ScenarioThresholdModes(TestHost &host) {
    SetKeep(host, true);
    SetSpace(host, OLMCK_SPACE_RGB);
    SetCount(host, 2);
    SetKeyColor(host, 0, 0, 0, 0);
    SetKeyColor(host, 1, 0, 0, 0);
    SetUseColor(host, 0, true);
    SetUseColor(host, 1, true);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 0, 0, 0, 1);
    FillRect(host, 1, 1, 1, 1, 0.1f, 0, 0, 1);
    FillRect(host, 3, 1, 3, 1, 0.04f, 0, 0, 1);
    FillRect(host, 5, 1, 5, 1, 0, 0.1f, 0, 1);

    SetPerColor(host, false);
    SetPerComponent(host, false);
    SetGlobalThr(host, 0.05f);
    if (!RenderChecked(host, "threshold neither")) return false;
    CHECK(PixelIs(host, host.output, 3, 1, EncDec(host, 0.04f), 0, 0, 1),
          "neither: 0.04 hit");
    CHECK(PixelIs(host, host.output, 1, 1, EncDec(host, 0.1f), 0, 0, 0),
          "neither: 0.1 miss");
    CHECK(PixelIs(host, host.output, 5, 1, 0, EncDec(host, 0.1f), 0, 0),
          "neither: G 0.1 miss");

    SetPerColor(host, true);
    SetPerComponent(host, false);
    SetSlotThr(host, 0, 0.05f);
    SetSlotThr(host, 1, 0.3f);
    if (!RenderChecked(host, "threshold perColor")) return false;
    CHECK(PixelIs(host, host.output, 1, 1, EncDec(host, 0.1f), 0, 0, 1),
          "perColor: slot1 scalar 0.3 hits");
    CHECK(PixelIs(host, host.output, 3, 1, EncDec(host, 0.04f), 0, 0, 1),
          "perColor: slot0 0.05 hits 0.04");
    CHECK(PixelIs(host, host.output, 5, 1, 0, EncDec(host, 0.1f), 0, 1),
          "perColor aggregate ignores per-channel spread");

    SetPerColor(host, false);
    SetPerComponent(host, true);
    SetGlobalThrRGB(host, 0.05f, 0.3f, 0.05f);
    if (!RenderChecked(host, "threshold perComponent")) return false;
    CHECK(PixelIs(host, host.output, 5, 1, 0, EncDec(host, 0.1f), 0, 1),
          "perComponent: G 0.1 <= 0.3 hit");
    CHECK(PixelIs(host, host.output, 1, 1, EncDec(host, 0.1f), 0, 0, 0),
          "perComponent: R 0.1 > 0.05 miss");

    SetPerColor(host, true);
    SetPerComponent(host, true);
    SetSlotThrRGB(host, 0, 0.05f, 0.3f, 0.05f);
    SetSlotThrRGB(host, 1, 0.0f, 0.0f, 0.0f);
    if (!RenderChecked(host, "threshold both")) return false;
    CHECK(PixelIs(host, host.output, 5, 1, 0, EncDec(host, 0.1f), 0, 1),
          "both: slot0 G 0.1 hit");
    CHECK(PixelIs(host, host.output, 1, 1, EncDec(host, 0.1f), 0, 0, 0),
          "both: slot0 R 0.1 miss");
    std::printf("threshold modes OK\n");
    return true;
}

bool ScenarioPremultiplied(TestHost &host, const char *label) {
    SetKeep(host, true);
    SetKeyColor(host, 0, 0.501961f, 0.501961f, 0.501961f);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 0, 0, 0, 1);
    FillRect(host, 1, 1, 1, 1, 1, 1, 1, 0.501961f);
    SetPremul(host, true);
    if (!RenderChecked(host, label)) return false;
    CHECK(PixelIs(host, host.output, 1, 1, 1, 1, 1, 0.501961f),
          "premultiplied match hits (alpha preserved)");
    SetPremul(host, false);
    if (!RenderChecked(host, label)) return false;
    CHECK(PixelIs(host, host.output, 1, 1, 1, 1, 1, 0), "straight RGB does not match");
    std::printf("%s OK\n", label);
    return true;
}

bool ScenarioEpsilon(TestHost &host) {
    SetKeep(host, true);
    SetSpace(host, OLMCK_SPACE_RGB);
    SetGlobalThr(host, 0.0f);
    SetKeyColor(host, 0, 0, 0, 0);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 0, 0, 0, 1);
    const float one_code = 1.0f / 32768.0f;
    FillRect(host, 1, 1, 1, 1, one_code, 0, 0, 1);
    if (!RenderChecked(host, "epsilon Full 16-bit")) return false;
    CHECK(PixelIs(host, host.output, 1, 1, one_code, 0, 0, 0),
          "16-bit: one-code diff misses at eps=1/65536");
    host.params[OLMCK_FORCE_PRECISION].u.pd.value = OLMCK_PREC_8;
    if (!RenderChecked(host, "epsilon Force8 on 16-bit")) return false;
    CHECK(PixelIs(host, host.output, 1, 1, one_code, 0, 0, 1),
          "Force 8bit upgrades epsilon to 1/510 -> hit");
    host.params[OLMCK_FORCE_PRECISION].u.pd.value = OLMCK_PREC_FULL;
    std::printf("epsilon selection OK\n");
    return true;
}

bool ScenarioThin(TestHost &host, const char *label) {
    SetKeep(host, true);
    SetSpace(host, OLMCK_SPACE_RGB);
    SetKeyColor(host, 0, 1, 0, 0);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 1, 1, 1, 1);
    FillRect(host, 3, 1, 7, 5, 1, 0, 0, 1);

    SetThin(host, -1, OLMCK_DIST_BOX);
    if (!RenderChecked(host, label)) return false;
    CHECK(PixelIs(host, host.output, 3, 1, 1, 0, 0, 0), "erode: ring corner cleared");
    CHECK(PixelIs(host, host.output, 5, 1, 1, 0, 0, 0), "erode: ring edge cleared");
    CHECK(PixelIs(host, host.output, 4, 2, 1, 0, 0, 1), "erode: distance 1 kept");
    CHECK(PixelIs(host, host.output, 5, 3, 1, 0, 0, 1), "erode: center kept");
    CHECK(PixelIs(host, host.output, 0, 0, 1, 1, 1, 0), "erode: outside untouched");

    SetThin(host, 1, OLMCK_DIST_BOX);
    if (!RenderChecked(host, label)) return false;
    // With equal weights the box chamfer is the Chebyshev metric (diagonal
    // moves cost wy = 1), so every 8-neighbour of the ring restores.
    CHECK(PixelIs(host, host.output, 2, 3, 1, 1, 1, 1), "grow: left neighbour restored");
    CHECK(PixelIs(host, host.output, 8, 3, 1, 1, 1, 1), "grow: right neighbour restored");
    CHECK(PixelIs(host, host.output, 5, 0, 1, 1, 1, 1), "grow: above restored");
    CHECK(PixelIs(host, host.output, 5, 6, 1, 1, 1, 1), "grow: below restored");
    CHECK(PixelIs(host, host.output, 2, 0, 1, 1, 1, 1),
          "grow: diagonal restored (box diagonal costs wy = 1)");
    CHECK(PixelIs(host, host.output, 1, 0, 1, 1, 1, 0),
          "grow: Chebyshev distance 2 not restored");
    CHECK(PixelIs(host, host.output, 5, 3, 1, 0, 0, 1), "grow: center intact");
    std::printf("%s OK\n", label);
    return true;
}

bool ScenarioThinEuclidRestore(TestHost &host) {
    SetKeep(host, true);
    SetKeyColor(host, 0, 1, 0, 0);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 1, 1, 1, 1);
    FillRect(host, 3, 1, 7, 5, 1, 0, 0, 1);
    SetThin(host, 2, OLMCK_DIST_EUCLIDEAN);
    if (!RenderChecked(host, "Euclid thin grow 2")) return false;
    CHECK(PixelIs(host, host.output, 2, 0, 1, 1, 1, 1),
          "Euclid: diagonal at sqrt(2) <= 2 restored");
    CHECK(PixelIs(host, host.output, 1, 0, 1, 1, 1, 0),
          "Euclid: distance sqrt(5) > 2 not restored");
    std::printf("Euclid thin restore OK\n");
    return true;
}

bool ScenarioDistancesNonSquare(TestHost &host) {
    host.in.downsample_x.den = 2;
    host.in.downsample_y.den = 4;
    SetKeep(host, true);
    SetKeyColor(host, 0, 1, 0, 0);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 1, 1, 1, 1);
    FillRect(host, 4, 3, 4, 3, 1, 0, 0, 1);

    SetThin(host, 3, OLMCK_DIST_EUCLIDEAN);
    if (!RenderChecked(host, "Euclid den(2,4) thin 3")) return false;
    CHECK(PixelIs(host, host.output, 3, 3, 1, 1, 1, 1), "Euclid wx=2: left restored");
    CHECK(PixelIs(host, host.output, 5, 3, 1, 1, 1, 1), "Euclid wx=2: right restored");
    CHECK(PixelIs(host, host.output, 4, 2, 1, 1, 1, 0), "Euclid wy=4: above not restored");
    CHECK(PixelIs(host, host.output, 4, 4, 1, 1, 1, 0), "Euclid wy=4: below not restored");

    SetThin(host, 5, OLMCK_DIST_EUCLIDEAN);
    if (!RenderChecked(host, "Euclid den(2,4) thin 5")) return false;
    CHECK(PixelIs(host, host.output, 4, 2, 1, 1, 1, 1), "Euclid thin5: above restored");
    CHECK(PixelIs(host, host.output, 3, 2, 1, 1, 1, 1),
          "Euclid thin5: diagonal sqrt(20) restored");

    // Box, thin=5: with wx=2/wy=4 a diagonal step costs wy, so the up-left
    // neighbour is at distance 4 and restores; the (2,2) pixel needs one
    // diagonal (4) plus one horizontal (2) = 6, and two vertical steps are 8.
    SetThin(host, 5, OLMCK_DIST_BOX);
    if (!RenderChecked(host, "Box den(2,4) thin 5")) return false;
    CHECK(PixelIs(host, host.output, 4, 2, 1, 1, 1, 1), "Box wx2/wy4: above dist 4 restored");
    CHECK(PixelIs(host, host.output, 3, 2, 1, 1, 1, 1),
          "Box: diagonal step costs wy=4 -> restored");
    CHECK(PixelIs(host, host.output, 2, 2, 1, 1, 1, 0),
          "Box: diagonal+horizontal 4+2=6 not restored");
    CHECK(PixelIs(host, host.output, 4, 1, 1, 1, 1, 0),
          "Box: two vertical steps 8 not restored");

    // Approx, thin=5: only axis moves, so the diagonal is wx+wy = 6, while two
    // horizontal steps cost 2*wx = 4.
    SetThin(host, 5, OLMCK_DIST_APPROX);
    if (!RenderChecked(host, "Approx den(2,4) thin 5")) return false;
    CHECK(PixelIs(host, host.output, 4, 2, 1, 1, 1, 1), "Approx: above dist 4 restored");
    CHECK(PixelIs(host, host.output, 3, 2, 1, 1, 1, 0),
          "Approx diagonal dist 6 not restored");
    CHECK(PixelIs(host, host.output, 2, 3, 1, 1, 1, 1),
          "Approx: two horizontal steps 4 restored");

    host.in.downsample_x.den = 1;
    host.in.downsample_y.den = 1;
    std::printf("non-square distances OK\n");
    return true;
}

bool ScenarioNoSeed(TestHost &host) {
    SetKeep(host, true);
    SetKeyColor(host, 0, 1, 1, 1);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 1, 1, 1, 1);
    SetThin(host, -4000, OLMCK_DIST_BOX);
    if (!RenderChecked(host, "no-seed box")) return false;
    CHECK(PixelIs(host, host.output, 0, 0, 1, 1, 1, 0), "box no-seed: corner at 3999 cleared");
    CHECK(PixelIs(host, host.output, 1, 0, 1, 1, 1, 1), "box no-seed: clamped 4000 kept");
    SetThin(host, -4000, OLMCK_DIST_EUCLIDEAN);
    if (!RenderChecked(host, "no-seed euclid")) return false;
    CHECK(PixelIs(host, host.output, 0, 0, 1, 1, 1, 1), "euclid no-seed: nothing cleared");
    std::printf("no-seed distances OK\n");
    return true;
}

bool ScenarioBlur(TestHost &host, const char *label) {
    SetKeep(host, true);
    SetKeyColor(host, 0, 1, 0, 0);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 1, 1, 1, 1);
    FillRect(host, 3, 1, 7, 5, 1, 0, 0, 1);
    SetBlur(host, 2.0f, OLMCK_DIST_BOX, OLMCK_DIR_INSIDE);
    if (!RenderChecked(host, label)) return false;
    CHECK(PixelIs(host, host.output, 3, 1, 1, 0, 0, 0), "blur inside: ring zeroed");
    CHECK(PixelIs(host, host.output, 4, 2, 1, 0, 0, EncDec(host, 0.5f)),
          "blur inside: dist1 half");
    CHECK(PixelIs(host, host.output, 5, 3, 1, 0, 0, 1), "blur inside: center full");
    CHECK(PixelIs(host, host.output, 0, 0, 1, 1, 1, 0), "blur inside: outside zero");

    SetBlur(host, 2.0f, OLMCK_DIST_BOX, OLMCK_DIR_AROUND);
    if (!RenderChecked(host, label)) return false;
    CHECK(PixelIs(host, host.output, 3, 1, 1, 0, 0, EncDec(host, 0.5f)),
          "blur around: ring half");
    CHECK(PixelIs(host, host.output, 4, 2, 1, 0, 0, EncDec(host, 0.8535534f)),
          "blur around: inner 0.85355");
    CHECK(PixelIs(host, host.output, 5, 3, 1, 0, 0, 1), "blur around: center full");
    CHECK(PixelIs(host, host.output, 2, 3, 1, 1, 1, EncDec(host, 0.1464466f)),
          "blur around: outer neighbour 0.14645");
    CHECK(PixelIs(host, host.output, 1, 3, 1, 1, 1, 0), "blur around: far outside zero");

    SetBlur(host, 2.0f, OLMCK_DIST_BOX, OLMCK_DIR_OUTSIDE);
    if (!RenderChecked(host, label)) return false;
    CHECK(PixelIs(host, host.output, 5, 3, 1, 0, 0, 1), "blur outside: keyed unchanged");
    CHECK(PixelIs(host, host.output, 2, 3, 1, 1, 1, EncDec(host, 0.5f)),
          "blur outside: neighbour half");
    CHECK(PixelIs(host, host.output, 1, 3, 1, 1, 1, 0), "blur outside: far outside zero");
    std::printf("%s OK\n", label);
    return true;
}

bool ScenarioKeepInvertAfterBlur(TestHost &host) {
    SetKeep(host, false);
    SetKeyColor(host, 0, 1, 0, 0);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 1, 1, 1, 1);
    FillRect(host, 3, 1, 7, 5, 1, 0, 0, 1);
    SetBlur(host, 2.0f, OLMCK_DIST_BOX, OLMCK_DIR_INSIDE);
    if (!RenderChecked(host, "keep inversion after blur")) return false;
    CHECK(PixelIs(host, host.output, 3, 1, 1, 0, 0, 1), "invert: ring back to full");
    CHECK(PixelIs(host, host.output, 4, 2, 1, 0, 0, InvDec(host, 1.0f, 0.5f)),
          "invert: 255-127 = 128/255 (channel units)");
    CHECK(PixelIs(host, host.output, 5, 3, 1, 0, 0, 0), "invert: center fully keyed out");
    CHECK(PixelIs(host, host.output, 0, 0, 1, 1, 1, 1), "invert: outside untouched");
    std::printf("keep inversion after blur OK\n");
    return true;
}

bool ScenarioExtentEarlyOut(TestHost &host) {
    // Early-out path (no thin/blur): the keyer iterates only the output
    // extent (the checked-out output world's extent_hint, which AE sets to
    // the render area); outside it the output keeps the initial input copy.
    SetKeep(host, true);
    SetKeyColor(host, 0, 1, 0, 0);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 1, 1, 1, 1);
    FillRect(host, 2, 2, 6, 4, 1, 0, 0, 1);
    PF_Rect request{2, 1, 6, 5};
    const PF_Rect saved_hint = host.output.extent_hint;
    host.output.extent_hint = request;
    reset_output(host);
    const int co_before = host.param_checkouts;
    const int ci_before = host.param_checkins;
    const int lc_before = host.layer_checkouts;
    const int li_before = host.layer_checkins;
    const int oc_before = host.output_checkouts;
    PF_Rect result{0, 0, 0, 0};
    if (!run_pre_render(host, request, &result)) {
        std::printf("FAIL: extent pre-render\n");
        ++g_failures;
        return false;
    }
    if (run_render(host, request) != PF_Err_NONE) {
        std::printf("FAIL: extent render errored\n");
        ++g_failures;
        return false;
    }
    host.output.extent_hint = saved_hint;
    CHECK(host.param_checkouts - co_before == host.param_checkins - ci_before &&
              host.layer_checkouts - lc_before == 1 &&
              host.layer_checkins - li_before == 1 &&
              host.output_checkouts - oc_before == 1,
          "extent: balance");
    CHECK(PixelIs(host, host.output, 4, 3, 1, 0, 0, 1), "extent: keyed inside extent");
    CHECK(PixelIs(host, host.output, 0, 0, 1, 1, 1, 1),
          "extent early-out: outside extent keeps the copy");
    CHECK(PixelIs(host, host.output, 7, 0, 1, 1, 1, 1),
          "extent early-out: far corner keeps the copy");
    std::printf("sub-extent early-out OK\n");
    return true;
}

bool ScenarioExtentCanvas(TestHost &host) {
    // Canvas path: canvas A starts cleared, so pixels far outside the extent
    // end up zero in the output (binary behaviour of the full-world copy).
    SetKeep(host, true);
    SetKeyColor(host, 0, 1, 0, 0);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 1, 1, 1, 1);
    FillRect(host, 3, 2, 5, 4, 1, 0, 0, 1);
    SetThin(host, 1, OLMCK_DIST_BOX);
    PF_Rect request{2, 1, 6, 5};
    const PF_Rect saved_hint = host.output.extent_hint;
    host.output.extent_hint = request;
    reset_output(host);
    const int co_before = host.param_checkouts;
    const int ci_before = host.param_checkins;
    const int lc_before = host.layer_checkouts;
    const int li_before = host.layer_checkins;
    const int oc_before = host.output_checkouts;
    PF_Rect result{0, 0, 0, 0};
    if (!run_pre_render(host, request, &result)) {
        std::printf("FAIL: extent canvas pre-render\n");
        ++g_failures;
        return false;
    }
    if (run_render(host, request) != PF_Err_NONE) {
        std::printf("FAIL: extent canvas render errored\n");
        ++g_failures;
        return false;
    }
    host.output.extent_hint = saved_hint;
    CHECK(host.param_checkouts - co_before == host.param_checkins - ci_before &&
              host.layer_checkouts - lc_before == 1 &&
              host.layer_checkins - li_before == 1 &&
              host.output_checkouts - oc_before == 1,
          "extent canvas: balance");
    CHECK(PixelIs(host, host.output, 4, 3, 1, 0, 0, 1), "extent canvas: keyed inside");
    CHECK(PixelIs(host, host.output, 0, 0, 0, 0, 0, 0),
          "extent canvas: far outside extent is zero (cleared canvas copied)");
    std::printf("sub-extent canvas OK\n");
    return true;
}

bool ScenarioPaddedRows(TestHost &host) {
    // Padded input rows AND padded NewWorld rows: everything must keep using
    // world.rowbytes.
    SetKeep(host, true);
    SetKeyColor(host, 0, 1, 0, 0);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 1, 1, 1, 1);
    FillRect(host, 2, 2, 6, 4, 1, 0, 0, 1);
    if (!RenderChecked(host, "padded rows")) return false;
    CHECK(PixelIs(host, host.output, 4, 3, 1, 0, 0, 1), "padded: keyed hit");
    CHECK(PixelIs(host, host.output, 0, 0, 1, 1, 1, 0), "padded: miss");
    std::printf("padded row stride OK\n");
    return true;
}

bool ScenarioCountRejection(TestHost &host) {
    // count = 26 > 25: slot 26's parameters do not exist; the checkout must
    // fail and the render must error (no OOB access, no silent pass-through).
    SetKeep(host, true);
    SetCount(host, 26);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 1, 1, 1, 1);
    reset_output(host);
    PF_Rect request{0, 0, kWidth, kHeight};
    const PF_Err err = run_render(host, request);
    CHECK(err != PF_Err_NONE, "count 26 must be rejected with an error");
    CHECK(host.layer_checkouts == 1 && host.layer_checkins == 1,
          "input layer still checked in after the failed checkout");
    SetCount(host, 1);
    std::printf("count>25 rejection OK\n");
    return true;
}

bool ScenarioLabRepeatedShift(TestHost &host) {
    // Lab76 shifts key and pixel by (+133.037, +163.48801) on EVERY call and
    // the caller never resets the pixel triple between slots, so after a
    // non-matching slot 0 the pixel carries one extra shift when slot 1 is
    // compared: diff = (pixel - key1) + (133.037, 163.48801). With key1
    // identical to the pixel the a/b delta is exactly the shift
    // (dist ~ sqrt(133.037^2 + 163.48801^2) ~ 210.8), far beyond the
    // (eps + 0.01) * 424.43527 ~ 4.3 limit -> miss. The same colours hit when
    // no earlier slot ran.
    SetKeep(host, true);
    SetSpace(host, OLMCK_SPACE_LAB76);
    SetPerColor(host, true);
    SetCount(host, 2);
    SetUseColor(host, 0, true);
    SetUseColor(host, 1, true);
    SetSlotThr(host, 0, 0.01f);
    SetSlotThr(host, 1, 0.01f);
    SetKeyColor(host, 0, 1, 0, 0);  // red: cannot match the gray pixel
    SetKeyColor(host, 1, 0.5f, 0.5f, 0.5f);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 0, 0, 0, 1);
    FillRect(host, 1, 1, 1, 1, 0.5f, 0.5f, 0.5f, 1);
    if (!RenderChecked(host, "Lab76 repeated shift")) return false;
    CHECK(PixelIs(host, host.output, 1, 1, EncDec(host, 0.5f), EncDec(host, 0.5f),
                  EncDec(host, 0.5f), 0),
          "Lab76: pixel shifted by slot 0's call, identical key1 misses");
    // Control: only slot 1 enabled (no prior shift) -> identical colours hit.
    SetCount(host, 1);
    SetKeyColor(host, 0, 0.5f, 0.5f, 0.5f);
    SetUseColor(host, 1, false);
    if (!RenderChecked(host, "Lab76 no prior shift")) return false;
    CHECK(PixelIs(host, host.output, 1, 1, EncDec(host, 0.5f), EncDec(host, 0.5f),
                  EncDec(host, 0.5f), 1),
          "Lab76: identical colours hit when no earlier slot ran");
    std::printf("Lab76 repeated-shift quirk OK\n");
    return true;
}

bool ScenarioLab94LightnessDelta(TestHost &host) {
    // Regression for the Lab94 aggregate term: grays share (near-)identical
    // chroma, so a lightness-only difference must dominate the distance.
    // Key gray 0.25 vs pixel gray 0.75: dL ~ 46.7, dC ~ 4.1. With the
    // (incorrect) dC^2 first term the distance would collapse to ~4.9 and
    // accept at threshold 0.05; the correct dL^2 term rejects there and only
    // accepts once (eps + thr) * 352.978 covers dL (thr >= ~0.133).
    SetKeep(host, true);
    SetSpace(host, OLMCK_SPACE_LAB94);
    SetPerColor(host, true);
    SetPerComponent(host, false);
    SetCount(host, 1);
    SetUseColor(host, 0, true);
    SetKeyColor(host, 0, 0.25f, 0.25f, 0.25f);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 0.25f, 0.25f, 0.25f, 1);
    FillRect(host, 1, 1, 1, 1, 0.75f, 0.75f, 0.75f, 1);
    SetSlotThr(host, 0, 0.05f);
    if (!RenderChecked(host, "Lab94 lightness reject")) return false;
    CHECK(PixelIs(host, host.output, 1, 1, EncDec(host, 0.75f), EncDec(host, 0.75f),
                  EncDec(host, 0.75f), 0),
          "Lab94: dL 46.7 rejects at thr 0.05 (limit 18.3)");
    CHECK(PixelIs(host, host.output, 0, 0, EncDec(host, 0.25f), EncDec(host, 0.25f),
                  EncDec(host, 0.25f), 1),
          "Lab94: identical key colour still hits");
    SetSlotThr(host, 0, 0.14f);
    if (!RenderChecked(host, "Lab94 lightness accept")) return false;
    CHECK(PixelIs(host, host.output, 1, 1, EncDec(host, 0.75f), EncDec(host, 0.75f),
                  EncDec(host, 0.75f), 1),
          "Lab94: dL 46.7 accepts at thr 0.14 (limit 50.1)");
    std::printf("Lab94 lightness-delta regression OK\n");
    return true;
}

// Runs on its own 8-bit host; verifies the smart-render error path keeps the
// pixel checkout accounting and world disposal balanced.
bool ScenarioSuiteFailureBalance(const char *fail_suite, bool canvas_path,
                                 const char *label) {
    TestHost host;
    if (!init_host(host, 8)) {
        std::printf("FAIL: %s host init\n", label);
        ++g_failures;
        return false;
    }
    host.fail_suite = fail_suite;
    SetKeep(host, true);
    SetKeyColor(host, 0, 1, 0, 0);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 1, 1, 1, 1);
    FillRect(host, 2, 2, 6, 4, 1, 0, 0, 1);
    if (canvas_path) SetThin(host, 1, OLMCK_DIST_BOX);
    reset_output(host);
    const int li_before = host.layer_checkins;
    const int lo_before = host.layer_checkouts;
    const int co_before = host.param_checkouts;
    PF_Rect request{0, 0, kWidth, kHeight};
    const PF_Err err = run_render(host, request);
    host.fail_suite = nullptr;
    CHECK(err != PF_Err_NONE, "injected suite failure must error the render");
    CHECK(host.layer_checkouts - lo_before == 1 && host.layer_checkins - li_before == 1,
          "layer pixels must be checked in exactly once despite the throw");
    if (canvas_path) {
        CHECK(host.world_buffers.empty(), "canvases must be disposed during unwinding");
    }
    (void)co_before;
    teardown_host(host);
    std::printf("%s OK\n", label);
    return true;
}

// Permissive fake host: out-of-range slot checkouts are SERVED, so only the
// explicit count guard can reject; verifies no slot parameter beyond the
// registered range is ever requested.
bool ScenarioCountGuardPermissive() {
    TestHost host;
    if (!init_host(host, 8)) {
        std::printf("FAIL: permissive host init\n");
        ++g_failures;
        return false;
    }
    host.permissive_checkout = true;
    SetKeep(host, true);
    SetCount(host, 26);
    FillRect(host, 0, 0, kWidth - 1, kHeight - 1, 1, 1, 1, 1);
    reset_output(host);
    PF_Rect request{0, 0, kWidth, kHeight};
    PF_Err err = run_render(host, request);
    CHECK(err == PF_Err_BAD_CALLBACK_PARAM,
          "count 26 must be rejected by the explicit guard even on a permissive host");
    CHECK(host.max_checkout_position < OLMCK_NUM_PARAMS,
          "no checkout may target a position beyond the registered parameters");
    SetCount(host, -1);
    reset_output(host);
    err = run_render(host, request);
    host.permissive_checkout = false;
    CHECK(err == PF_Err_BAD_CALLBACK_PARAM, "count -1 must be rejected as well");
    teardown_host(host);
    std::printf("permissive-host count guard OK\n");
    return true;
}

// Box chamfer on 1-pixel-wide/high canvases: plain 1D distance.
bool run_1d_chamfer_tests() {
    {  // 1x5 column, seed in the middle
        TestHost host;
        if (!init_host(host, 8, false, 1, 5)) {
            std::printf("FAIL: 1x5 host init\n");
            ++g_failures;
            return false;
        }
        SetKeep(host, true);
        SetKeyColor(host, 0, 1, 0, 0);
        FillRect(host, 0, 0, 0, 4, 1, 1, 1, 1);
        FillRect(host, 0, 2, 0, 2, 1, 0, 0, 1);
        SetThin(host, 1, OLMCK_DIST_BOX);
        if (!RenderChecked(host, "1x5 seeded")) return false;
        CHECK(PixelIs(host, host.output, 0, 1, 1, 1, 1, 1), "1x5: dist1 above restored");
        CHECK(PixelIs(host, host.output, 0, 3, 1, 1, 1, 1), "1x5: dist1 below restored");
        CHECK(PixelIs(host, host.output, 0, 0, 1, 1, 1, 0), "1x5: dist2 top not restored");
        CHECK(PixelIs(host, host.output, 0, 4, 1, 1, 1, 0), "1x5: dist2 bottom not restored");
        CHECK(PixelIs(host, host.output, 0, 2, 1, 0, 0, 1), "1x5: seed intact");
        teardown_host(host);
    }
    {  // 5x1 row, seed in the middle
        TestHost host;
        if (!init_host(host, 8, false, 5, 1)) {
            std::printf("FAIL: 5x1 host init\n");
            ++g_failures;
            return false;
        }
        SetKeep(host, true);
        SetKeyColor(host, 0, 1, 0, 0);
        FillRect(host, 0, 0, 4, 0, 1, 1, 1, 1);
        FillRect(host, 2, 0, 2, 0, 1, 0, 0, 1);
        SetThin(host, 1, OLMCK_DIST_BOX);
        if (!RenderChecked(host, "5x1 seeded")) return false;
        CHECK(PixelIs(host, host.output, 1, 0, 1, 1, 1, 1), "5x1: dist1 left restored");
        CHECK(PixelIs(host, host.output, 3, 0, 1, 1, 1, 1), "5x1: dist1 right restored");
        CHECK(PixelIs(host, host.output, 0, 0, 1, 1, 1, 0), "5x1: dist2 left not restored");
        CHECK(PixelIs(host, host.output, 4, 0, 1, 1, 1, 0), "5x1: dist2 right not restored");
        teardown_host(host);
    }
    {  // 1x5 with no seeds at all: forward big/cap, nothing to propagate back
        TestHost host;
        if (!init_host(host, 8, false, 1, 5)) {
            std::printf("FAIL: 1x5 no-seed host init\n");
            ++g_failures;
            return false;
        }
        SetKeep(host, true);
        SetKeyColor(host, 0, 1, 1, 1);  // everything matches -> no boundary
        FillRect(host, 0, 0, 0, 4, 1, 1, 1, 1);
        SetThin(host, -4000, OLMCK_DIST_BOX);
        if (!RenderChecked(host, "1x5 no-seed")) return false;
        CHECK(PixelIs(host, host.output, 0, 0, 1, 1, 1, 0),
              "1x5 no-seed: first pixel at big (3999) cleared by thin -4000");
        CHECK(PixelIs(host, host.output, 0, 1, 1, 1, 1, 1),
              "1x5 no-seed: capped 4000 kept");
        teardown_host(host);
    }
    {  // 1x5 seeded with non-square downsample: the step is wy
        TestHost host;
        if (!init_host(host, 8, false, 1, 5)) {
            std::printf("FAIL: 1x5 den host init\n");
            ++g_failures;
            return false;
        }
        host.in.downsample_y.den = 4;
        SetKeep(host, true);
        SetKeyColor(host, 0, 1, 0, 0);
        FillRect(host, 0, 0, 0, 4, 1, 1, 1, 1);
        FillRect(host, 0, 2, 0, 2, 1, 0, 0, 1);
        SetThin(host, 3, OLMCK_DIST_BOX);
        if (!RenderChecked(host, "1x5 den(4) thin 3")) return false;
        CHECK(PixelIs(host, host.output, 0, 1, 1, 1, 1, 0),
              "1x5 den4: dist 4 > 3 not restored");
        SetThin(host, 5, OLMCK_DIST_BOX);
        if (!RenderChecked(host, "1x5 den(4) thin 5")) return false;
        CHECK(PixelIs(host, host.output, 0, 1, 1, 1, 1, 1),
              "1x5 den4: dist 4 <= 5 restored");
        CHECK(PixelIs(host, host.output, 0, 3, 1, 1, 1, 1),
              "1x5 den4: dist 4 <= 5 restored below");
        host.in.downsample_y.den = 1;
        teardown_host(host);
    }
    std::printf("1D box chamfer (1xN / Nx1, seeded + no-seed) OK\n");
    return true;
}

bool run_all_for_depth(int bitdepth, bool pad_rows) {
    TestHost host;
    host.pad_new_worlds = pad_rows;
    if (!init_host(host, bitdepth, pad_rows)) {
        std::printf("FAIL: %d-bit host init\n", bitdepth);
        ++g_failures;
        return false;
    }
    bool ok = true;
#define RUN_SCENARIO(call) \
    do { \
        ResetParams(host); \
        ok = (call) && ok; \
    } while (0)
    RUN_SCENARIO(ScenarioKeyOutBlack(
        host, bitdepth == 8 ? "8-bit key-out black"
                            : bitdepth == 16 ? "16-bit key-out black"
                                             : "32-bit key-out black"));
    RUN_SCENARIO(ScenarioKeep(host, "keep ON"));
    RUN_SCENARIO(ScenarioPartialAlphaAndRgb(host, "partial alpha / RGB preserved"));
    RUN_SCENARIO(ScenarioReplace(host, "replacement"));
    RUN_SCENARIO(ScenarioReplaceGates(host));
    RUN_SCENARIO(ScenarioFirstHit(host, "first enabled hit"));
    RUN_SCENARIO(ScenarioColorSpaces(host));
    RUN_SCENARIO(ScenarioThresholdModes(host));
    RUN_SCENARIO(ScenarioPremultiplied(host, "premultiplied"));
    RUN_SCENARIO(ScenarioThin(host, "thin erosion/growth"));
    RUN_SCENARIO(ScenarioThinEuclidRestore(host));
    RUN_SCENARIO(ScenarioDistancesNonSquare(host));
    RUN_SCENARIO(ScenarioNoSeed(host));
    RUN_SCENARIO(ScenarioBlur(host, "blur profiles"));
    RUN_SCENARIO(ScenarioKeepInvertAfterBlur(host));
    RUN_SCENARIO(ScenarioExtentEarlyOut(host));
    RUN_SCENARIO(ScenarioExtentCanvas(host));
    if (pad_rows) RUN_SCENARIO(ScenarioPaddedRows(host));
    if (bitdepth == 16) RUN_SCENARIO(ScenarioEpsilon(host));
    RUN_SCENARIO(ScenarioLabRepeatedShift(host));
    RUN_SCENARIO(ScenarioLab94LightnessDelta(host));
#undef RUN_SCENARIO
    teardown_host(host);
    return ok;
}

}  // namespace

int main() {
    // Binary layout cross-check: the Windows binary's chamfer/EDT weights read
    // in_data+288 and in_data+296, and its checkout reads current_time at
    // +224 / time_scale at +240. That identifies +288/+296 as the DEN fields
    // of downsample_x/downsample_y (num/den pairs, 8 bytes apart). The offsets
    // below print the 26.5 SDK layout; the relative-order check catches any
    // field inserted between current_time and the downsample rationals.
    {
        const size_t ct = offsetof(PF_InData, current_time);
        const size_t dx = offsetof(PF_InData, downsample_x);
        const size_t dy = offsetof(PF_InData, downsample_y);
        std::printf("PF_InData offsets: current_time=%lu downsample_x=%lu "
                    "downsample_y=%lu (binary SDK: current_time +224, ds_x +284, "
                    "ds_y +292)\n",
                    (unsigned long)ct, (unsigned long)dx, (unsigned long)dy);
        std::fflush(stdout);
        if (dx - ct != 60 || dy - dx != 8) {
            std::printf("FAIL: PF_InData field order changed between current_time and "
                        "the downsample rationals; re-verify the chamfer weight "
                        "mapping (ds_x.den/ds_y.den) against the binary\n");
            ++g_failures;
        }
    }

    if (!verify_setup()) return 1;
    if (!verify_ui()) return 1;

    for (int bitdepth = 8; bitdepth <= 32; bitdepth *= 2) {
        if (!run_all_for_depth(bitdepth, false)) return 1;
    }
    if (!run_all_for_depth(8, true)) return 1;  // padded rows variant

    // count > 25 rejection (8-bit host is enough).
    {
        TestHost host;
        if (!init_host(host, 8)) return 1;
        if (!ScenarioCountRejection(host)) return 1;
        teardown_host(host);
    }
    if (!ScenarioCountGuardPermissive()) return 1;

    // Suite-failure injection: the layer checkin and world disposal must stay
    // balanced when CheckoutParams (color suite) or the kernel (iterate suite)
    // throw.
    if (!ScenarioSuiteFailureBalance(kPFColorParamSuite, false,
                                     "color-suite failure balance")) return 1;
    if (!ScenarioSuiteFailureBalance(kPFIterate8Suite, true,
                                     "iterate-suite failure balance (canvas path)"))
        return 1;

    if (!run_1d_chamfer_tests()) return 1;

    std::printf("PASS: %d checks. 224 params, UI modes, key/keep/replace, six color\n"
                "spaces, threshold modes, premultiply, epsilon, thin/blur/EDT,\n"
                "extents, padded rows, 8/16/32 bpc.\n",
                g_checks);
    return g_failures ? 1 : 0;
}

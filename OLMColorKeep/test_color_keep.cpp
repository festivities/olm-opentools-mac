// OLMColorKeep test: fake AE host driving the production EffectMain (OLMColorKeep.cpp).
// Expected values are the verified 1.1.0 binary spec (see the spec file in %TEMP%\opencode\specs);
// the binary-differential run is a separate harness. Suites are faked in-process; no AE needed.
// Build (out-of-source, MinGW + Win 26.5 SDK): see .agents/AGENTS.md test recipe.
#ifndef OLMCKP_PORT_CPP
#define OLMCKP_PORT_CPP "OLMColorKeep.cpp"
#endif
#include OLMCKP_PORT_CPP


#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <list>
#include <map>
#include <string>
#include <vector>
#include <random>
#include <limits>
#include "AE_ComputeCacheSuite.h"

namespace {

// ---------------------------------------------------------------- logging ---
int g_fail = 0;
#define EXPECT(cond, ...)                                  \
    do {                                                   \
        if (!(cond)) {                                     \
            ++g_fail;                                      \
            if (g_fail <= 60) {                            \
                std::printf("FAIL line %d: ", __LINE__);   \
                std::printf(__VA_ARGS__);                  \
                std::printf("\n");                         \
            }                                              \
        }                                                  \
    } while (0)

constexpr int kNumParams = OLMCKP_NUM_PARAMS;  // 224 (position 0 is the input layer)
constexpr size_t kGuard = 4096;

// ------------------------------------------------------------ side state ----
struct Side;
Side *g_cur = nullptr;
bool g_log_acquire = false;

struct Side {
    bool binary = false;
    const char *label = "";
    PF_InData in{};
    PF_OutData out{};
    PF_UtilCallbacks utils{};
    PF_ParamDef input_def{};
    std::array<PF_ParamDef, kNumParams> defaults{};  // as seen by add_param, by position
    std::array<PF_ParamDef, kNumParams> params{};    // live values, by position
    std::array<PF_ParamDef *, kNumParams> ptrs{};    // PF_UpdateParamUI-style params[] array
    int add_count = 0;
    std::vector<std::string> add_indices;            // add_param index arguments (info)
    std::vector<std::string> trace;                  // UI + stream events, ordered
    std::vector<std::string> acquires;               // "name vN ok|NS"
    int register_ui_calls = 0;
    std::string aegp_register_names;
    std::map<int, int> stream_state;                 // position -> last hidden value written
    std::map<int, int> stream_writes;
    std::map<int, int> ui_disabled;                  // position -> last PF_PUI_DISABLED
    std::map<int, PF_ParamDef> ui_last;              // position -> last def passed
    int checkout_calls = 0, checkin_calls = 0;
    int layer_checkouts = 0, layer_checkins = 0, output_checkouts = 0;
    int new_worlds = 0, dispose_worlds = 0;
    std::string newworld_log;
    size_t pixsize = 4;                              // bytes per pixel of the current render
    // render plumbing
    std::list<std::vector<unsigned char>> worlds;    // PF_NewWorld storage (stable addresses)
    std::vector<unsigned char> in_buf, out_buf;
    PF_EffectWorld in_world{}, out_world{};
    // SMART_PRE_RENDER record
    PF_RenderRequest pre_req{};
    A_long pre_time = 0, pre_step = 0;
    A_u_long pre_scale = 0;
    PF_ParamIndex pre_index = -1;
    A_long pre_cid = -1;
    int pre_calls = 0;
};

Side *g_sides[2] = {nullptr, nullptr};

// ------------------------------------------------------------ AE suites -----
A_Err fake_register(AEGP_GlobalRefcon, const A_char *name, AEGP_PluginID *id) {
    if (g_cur) {
        g_cur->aegp_register_names += name ? name : "?";
        g_cur->aegp_register_names += ";";
    }
    if (!id) return 1;
    *id = 0x1234;
    return A_Err_NONE;
}
AEGP_UtilitySuite3 g_util3{};
AEGP_UtilitySuite6 g_util6{};

A_Err fake_get_effect_layer(PF_ProgPtr, AEGP_LayerH *layer) {
    if (!layer) return 1;
    *layer = (AEGP_LayerH)0x1;
    return A_Err_NONE;
}
A_Err fake_get_new_effect_for_effect(AEGP_PluginID, PF_ProgPtr, AEGP_EffectRefH *effect) {
    if (!effect) return 1;
    *effect = (AEGP_EffectRefH)0x2;
    return A_Err_NONE;
}
A_Err fake_dispose_effect(AEGP_EffectRefH) { return A_Err_NONE; }
A_Err fake_get_stream(AEGP_PluginID, AEGP_EffectRefH, A_long index, AEGP_StreamRefH *stream) {
    if (!stream) return 1;
    *stream = (AEGP_StreamRefH)(ptrdiff_t)(index + 1);
    return A_Err_NONE;
}
A_Err fake_dispose_stream(AEGP_StreamRefH) { return A_Err_NONE; }
A_Err fake_set_dyn_flag(AEGP_StreamRefH stream, AEGP_DynStreamFlags flag, A_Boolean undoable,
                        A_Boolean value) {
    if (!g_cur) return 1;
    const int pos = (int)((ptrdiff_t)stream - 1);
    if (flag != AEGP_DynStreamFlag_HIDDEN) {
        g_cur->trace.push_back("DYNFLAG-NOT-HIDDEN");
        return A_Err_NONE;
    }
    g_cur->trace.push_back("HIDDEN pos=" + std::to_string(pos) + " hidden=" +
                           std::to_string(value ? 1 : 0) + " undoable=" + std::to_string(undoable ? 1 : 0));
    g_cur->stream_state[pos] = value ? 1 : 0;
    g_cur->stream_writes[pos]++;
    return A_Err_NONE;
}

AEGP_PFInterfaceSuite1 g_pfi{};
AEGP_ComputeCacheSuite1 g_cache{};
AEGP_EffectSuite4 g_effect{};
AEGP_StreamSuite5 g_stream{};
AEGP_DynamicStreamSuite3 g_dyn{};
// v4 (header id 5) inserts AEGP_GetNewStreamRefForMask at slot 1, so SetDynamicStreamFlag sits at
// slot 6 instead of 5: it needs its own struct (checked in AE_GeneralPlug.h / AE_GeneralPlugOld.h).
AEGP_DynamicStreamSuite4 g_dyn4{};
PF_ColorParamSuite1 g_color{};
PF_ParamUtilsSuite3 g_paramutils{};
PF_Iterate8Suite1 g_it8{};
PF_Iterate16Suite1 g_it16{};
PF_IterateFloatSuite1 g_itf{};
PF_WorldSuite2 g_world{};
SPBasicSuite g_basic{};

PF_Err get_color(PF_ProgPtr, const PF_ParamDef *def, PF_PixelFloat *out) {
    if (!def || !out) return PF_Err_BAD_CALLBACK_PARAM;
    out->red = (float)def->u.cd.value.red / 255.0f;
    out->green = (float)def->u.cd.value.green / 255.0f;
    out->blue = (float)def->u.cd.value.blue / 255.0f;
    out->alpha = (float)def->u.cd.value.alpha / 255.0f;
    return PF_Err_NONE;
}

PF_Err update_param_ui(PF_ProgPtr, PF_ParamIndex position, const PF_ParamDef *def) {
    if (!g_cur || !def) return PF_Err_BAD_CALLBACK_PARAM;
    g_cur->trace.push_back("UI pos=" + std::to_string(position) + " type=" + std::to_string((int)def->param_type) +
                           " ui=0x" + std::to_string((unsigned)def->ui_flags) + " flags=0x" +
                           std::to_string((unsigned)def->flags));
    g_cur->ui_disabled[(int)position] = (def->ui_flags & PF_PUI_DISABLED) ? 1 : 0;
    g_cur->ui_last[(int)position] = *def;
    return PF_Err_NONE;
}

PF_Err checkout_param(PF_ProgPtr, PF_ParamIndex position, A_long, A_long, A_u_long, PF_ParamDef *param) {
    if (!g_cur || !param) return PF_Err_BAD_CALLBACK_PARAM;
    g_cur->checkout_calls++;
    if (position <= 0 || position >= kNumParams) return PF_Err_BAD_CALLBACK_PARAM;
    *param = g_cur->params[(size_t)position];
    return PF_Err_NONE;
}

PF_Err checkin_param(PF_ProgPtr, PF_ParamDef *param) {
    if (!g_cur || !param) return PF_Err_BAD_CALLBACK_PARAM;
    g_cur->checkin_calls++;
    return PF_Err_NONE;
}

PF_Err add_param(PF_ProgPtr, PF_ParamIndex index, PF_ParamDefPtr def) {
    if (!g_cur || !def || g_cur->add_count >= kNumParams - 1) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    ++g_cur->add_count;
    const int pos = index > 0 ? (int)index : g_cur->add_count;
    g_cur->add_indices.push_back(std::to_string((int)index));
    g_cur->defaults[(size_t)pos] = *def;
    g_cur->params[(size_t)pos] = *def;
    return PF_Err_NONE;
}

PF_Err register_ui(PF_ProgPtr, PF_CustomUIInfo *) {
    if (g_cur) g_cur->register_ui_calls++;
    return PF_Err_NONE;
}

int SPAPI mock_sprintf(char *buffer, const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(buffer, 256, format, args);
    va_end(args);
    return written;
}

char *SPAPI mock_strcpy(char *d, const char *s) { return std::strcpy(d, s); }

// Copy: whole-world (min extent) or rect-to-rect when both rects are given.
PF_Err copy_world(PF_ProgPtr, PF_EffectWorld *src, PF_EffectWorld *dst, PF_Rect *src_r, PF_Rect *dst_r) {
    if (!src || !dst || !src->data || !dst->data) return PF_Err_BAD_CALLBACK_PARAM;
    A_long sx = 0, sy = 0, dx = 0, dy = 0;
    A_long w = std::min(src->width, dst->width), h = std::min(src->height, dst->height);
    if (src_r && dst_r) {
        sx = src_r->left; sy = src_r->top; dx = dst_r->left; dy = dst_r->top;
        w = std::min(src_r->right - src_r->left, dst_r->right - dst_r->left);
        h = std::min(src_r->bottom - src_r->top, dst_r->bottom - dst_r->top);
    }
    const size_t px = g_cur->pixsize;
    if (w <= 0 || h <= 0) return PF_Err_NONE;
    for (A_long y = 0; y < h; ++y) {
        std::memmove((char *)dst->data + (size_t)(dy + y) * dst->rowbytes + (size_t)dx * px,
                     (const char *)src->data + (size_t)(sy + y) * src->rowbytes + (size_t)sx * px,
                     (size_t)w * px);
    }
    return PF_Err_NONE;
}

template <typename Pixel>
PF_Err iterate_impl(PF_EffectWorld *src, PF_EffectWorld *dst, const PF_Rect *area,
                    PF_Err (*fn)(void *, A_long, A_long, Pixel *, Pixel *), void *refcon) {
    if (!src || !dst || !fn || !src->data || !dst->data) return PF_Err_BAD_CALLBACK_PARAM;
    A_long x0 = 0, y0 = 0;
    A_long x1 = std::min(src->width, dst->width), y1 = std::min(src->height, dst->height);
    if (area) {
        x0 = std::max(x0, area->left);
        y0 = std::max(y0, area->top);
        x1 = std::min(x1, area->right);
        y1 = std::min(y1, area->bottom);
    }
    for (A_long y = y0; y < y1; ++y) {
        for (A_long x = x0; x < x1; ++x) {
            Pixel *sp = (Pixel *)((char *)src->data + (size_t)y * src->rowbytes + (size_t)x * sizeof(Pixel));
            Pixel *dp = (Pixel *)((char *)dst->data + (size_t)y * dst->rowbytes + (size_t)x * sizeof(Pixel));
            PF_Err err = fn(refcon, x, y, sp, dp);
            if (err) return err;
        }
    }
    return PF_Err_NONE;
}

PF_Err iterate8(PF_InData *, A_long, A_long, PF_EffectWorld *src, const PF_Rect *area, void *refcon,
                PF_IteratePixel8Func fn, PF_EffectWorld *dst) {
    return iterate_impl<PF_Pixel>(src, dst, area, fn, refcon);
}
PF_Err iterate16(PF_InData *, A_long, A_long, PF_EffectWorld *src, const PF_Rect *area, void *refcon,
                 PF_IteratePixel16Func fn, PF_EffectWorld *dst) {
    return iterate_impl<PF_Pixel16>(src, dst, area, fn, refcon);
}
PF_Err iteratef(PF_InData *, A_long, A_long, PF_EffectWorld *src, const PF_Rect *area, void *refcon,
                PF_IteratePixelFloatFunc fn, PF_EffectWorld *dst) {
    return iterate_impl<PF_PixelFloat>(src, dst, area, fn, refcon);
}

PF_Err new_world(PF_ProgPtr, A_long width, A_long height, PF_Boolean clear, PF_PixelFormat format,
                 PF_EffectWorld *world) {
    if (!g_cur || !world || width < 0 || height < 0) return PF_Err_BAD_CALLBACK_PARAM;
    const A_long px = format == PF_PixelFormat_ARGB32 ? 4 : format == PF_PixelFormat_ARGB64 ? 8
                      : format == PF_PixelFormat_ARGB128 ? 16 : -1;
    if (px < 0) return PF_Err_BAD_CALLBACK_PARAM;
    const A_long rb = width * px;
    g_cur->worlds.emplace_back((size_t)rb * (size_t)std::max<A_long>(height, 1), clear ? (unsigned char)0 : (unsigned char)0xD7);
    std::vector<unsigned char> &buf = g_cur->worlds.back();
    AEFX_CLR_STRUCT(*world);
    world->data = (PF_PixelPtr)buf.data();
    world->rowbytes = rb;
    world->width = width;
    world->height = height;
    world->extent_hint.left = 0;
    world->extent_hint.top = 0;
    world->extent_hint.right = width;
    world->extent_hint.bottom = height;
    g_cur->new_worlds++;
    g_cur->newworld_log += std::to_string(width) + "x" + std::to_string(height) + "/" + std::to_string((int)clear) + "/" + std::to_string((int)format) + ";";
    return PF_Err_NONE;
}

PF_Err dispose_world(PF_ProgPtr, PF_EffectWorld *world) {
    if (!g_cur || !world) return PF_Err_BAD_CALLBACK_PARAM;
    g_cur->dispose_worlds++;
    for (auto it = g_cur->worlds.begin(); it != g_cur->worlds.end(); ++it) {
        if (it->data() == (unsigned char *)world->data) {
            g_cur->worlds.erase(it);
            return PF_Err_NONE;
        }
    }
    return PF_Err_BAD_CALLBACK_PARAM;
}

// PF Handle Suite v2 (PF_HandleSuite1 layout): the binary's GLOBAL_SETUP/PARAMS_SETUP need it.
struct HandleRec {
    void *mem;
    A_HandleSize size;
};
PF_Handle H_new(A_HandleSize n) {
    HandleRec *h = (HandleRec *)std::calloc(1, sizeof(HandleRec));
    h->size = n;
    h->mem = std::calloc(1, n ? (size_t)n : 1);
    return (PF_Handle)h;
}
void *H_lock(PF_Handle p) { return ((HandleRec *)p)->mem; }
void H_unlock(PF_Handle) {}
void H_dispose(PF_Handle p) {
    if (!p) return;
    std::free(((HandleRec *)p)->mem);
    std::free(p);
}
A_HandleSize H_size(PF_Handle p) { return ((HandleRec *)p)->size; }
PF_Err H_resize(A_HandleSize n, PF_Handle *hp) {
    HandleRec *h = (HandleRec *)*hp;
    void *m = std::realloc(h->mem, n ? (size_t)n : 1);
    if (!m) return PF_Err_OUT_OF_MEMORY;
    h->mem = m;
    h->size = n;
    return PF_Err_NONE;
}
PF_HandleSuite1 g_handle{};

// PF ANSI Suite v1 (the binary acquires it from ABOUT on; a NOT SUPPORTED answer makes it throw).
PF_ANSICallbacksBlock g_ansi{};
A_FpLong an_atan(A_FpLong x) { return std::atan(x); }
A_FpLong an_atan2(A_FpLong y, A_FpLong x) { return std::atan2(y, x); }
A_FpLong an_ceil(A_FpLong x) { return std::ceil(x); }
A_FpLong an_cos(A_FpLong x) { return std::cos(x); }
A_FpLong an_exp(A_FpLong x) { return std::exp(x); }
A_FpLong an_fabs(A_FpLong x) { return std::fabs(x); }
A_FpLong an_floor(A_FpLong x) { return std::floor(x); }
A_FpLong an_fmod(A_FpLong x, A_FpLong y) { return std::fmod(x, y); }
A_FpLong an_hypot(A_FpLong x, A_FpLong y) { return std::hypot(x, y); }
A_FpLong an_log(A_FpLong x) { return std::log(x); }
A_FpLong an_log10(A_FpLong x) { return std::log10(x); }
A_FpLong an_pow(A_FpLong x, A_FpLong y) { return std::pow(x, y); }
A_FpLong an_sin(A_FpLong x) { return std::sin(x); }
A_FpLong an_sqrt(A_FpLong x) { return std::sqrt(x); }
A_FpLong an_tan(A_FpLong x) { return std::tan(x); }
A_FpLong an_asin(A_FpLong x) { return std::asin(x); }
A_FpLong an_acos(A_FpLong x) { return std::acos(x); }
void init_ansi() {
    g_ansi.atan = an_atan;
    g_ansi.atan2 = an_atan2;
    g_ansi.ceil = an_ceil;
    g_ansi.cos = an_cos;
    g_ansi.exp = an_exp;
    g_ansi.fabs = an_fabs;
    g_ansi.floor = an_floor;
    g_ansi.fmod = an_fmod;
    g_ansi.hypot = an_hypot;
    g_ansi.log = an_log;
    g_ansi.log10 = an_log10;
    g_ansi.pow = an_pow;
    g_ansi.sin = an_sin;
    g_ansi.sqrt = an_sqrt;
    g_ansi.tan = an_tan;
    g_ansi.asin = an_asin;
    g_ansi.acos = an_acos;
    g_ansi.sprintf = mock_sprintf;
    g_ansi.strcpy = mock_strcpy;
}

SPErr SPAPI acquire_suite(const char *name, int32 version, const void **suite) {
    if (!name || !suite) return (SPErr)-1;
    const void *p = nullptr;
    if (!std::strcmp(name, kPFANSISuite) && version == kPFANSISuiteVersion1) p = &g_ansi;
    else if (!std::strcmp(name, kPFHandleSuite) && version == kPFHandleSuiteVersion1) p = &g_handle;
    else if (!std::strcmp(name, kPFColorParamSuite) && version == kPFColorParamSuiteVersion1) p = &g_color;
    else if (!std::strcmp(name, kPFParamUtilsSuite) && version == kPFParamUtilsSuiteVersion3) p = &g_paramutils;
    else if (!std::strcmp(name, kPFIterate8Suite) && version == kPFIterate8SuiteVersion1) p = &g_it8;
    else if (!std::strcmp(name, kPFIterate16Suite) && version == kPFIterate16SuiteVersion1) p = &g_it16;
    else if (!std::strcmp(name, kPFIterateFloatSuite) && version == kPFIterateFloatSuiteVersion1) p = &g_itf;
    else if (!std::strcmp(name, kPFWorldSuite) && version == kPFWorldSuiteVersion2) p = &g_world;
    else if (!std::strcmp(name, kAEGPUtilitySuite) && version == kAEGPUtilitySuiteVersion3) p = &g_util3;
    else if (!std::strcmp(name, kAEGPUtilitySuite) && version == kAEGPUtilitySuiteVersion6) p = &g_util6;
    else if (!std::strcmp(name, kAEGPPFInterfaceSuite) && version == kAEGPPFInterfaceSuiteVersion1) p = &g_pfi;
    else if (!std::strcmp(name, kAEGPComputeCacheSuite) && version == kAEGPComputeCacheSuiteVersion1) p = &g_cache;
    // Older versions share the member prefix the harness uses (verified in the Win SDK headers:
    // Stream v10 == v12 for the first 16 slots incl. GetNewEffectStreamByIndex/DisposeStream;
    // Effect v4 == v5 for the first 22 slots incl. DisposeEffect).
    else if (!std::strcmp(name, kAEGPEffectSuite) && (version == 4 || version == kAEGPEffectSuiteVersion5)) p = &g_effect;
    else if (!std::strcmp(name, kAEGPStreamSuite) && (version == 10 || version == kAEGPStreamSuiteVersion7)) p = &g_stream;
    else if (!std::strcmp(name, kAEGPDynamicStreamSuite) && version == kAEGPDynamicStreamSuiteVersion3) p = &g_dyn;
    else if (!std::strcmp(name, kAEGPDynamicStreamSuite) && version == kAEGPDynamicStreamSuiteVersion4) p = &g_dyn4;
    if (g_cur) {
        g_cur->acquires.push_back(std::string(name) + " v" + std::to_string((int)version) + (p ? " ok" : " NS"));
    }
    if (g_log_acquire)
        std::printf("      [%s] AcquireSuite(\"%s\", %d) -> %s\n", g_cur ? g_cur->label : "?", name, (int)version,
                    p ? "ok" : "NOT SUPPORTED");
    *suite = p;
    return p ? kSPNoError : (SPErr)1;
}
SPErr SPAPI release_suite(const char *, int32) { return kSPNoError; }

PF_Err checkout_layer_pixels(PF_ProgPtr, A_long, PF_EffectWorld **world) {
    if (!g_cur || !world) return PF_Err_BAD_CALLBACK_PARAM;
    g_cur->layer_checkouts++;
    *world = &g_cur->in_world;
    return PF_Err_NONE;
}
PF_Err checkin_layer_pixels(PF_ProgPtr, A_long) {
    if (!g_cur) return PF_Err_BAD_CALLBACK_PARAM;
    g_cur->layer_checkins++;
    return PF_Err_NONE;
}
PF_Err checkout_output(PF_ProgPtr, PF_EffectWorld **world) {
    if (!g_cur || !world) return PF_Err_BAD_CALLBACK_PARAM;
    g_cur->output_checkouts++;
    *world = &g_cur->out_world;
    return PF_Err_NONE;
}

// Pre-render checkout: the scenario supplies the result rects (set in g_pre_result).
PF_LRect g_chk_result{0, 0, 0, 0}, g_chk_max{0, 0, 0, 0};
PF_Err pre_checkout(PF_ProgPtr, PF_ParamIndex index, A_long checkout_id, const PF_RenderRequest *req,
                    A_long time, A_long step, A_u_long scale, PF_CheckoutResult *res) {
    if (!g_cur || !req || !res) return PF_Err_BAD_CALLBACK_PARAM;
    g_cur->pre_calls++;
    g_cur->pre_req = *req;
    g_cur->pre_time = time;
    g_cur->pre_step = step;
    g_cur->pre_scale = scale;
    g_cur->pre_index = index;
    g_cur->pre_cid = checkout_id;
    res->result_rect = g_chk_result;
    res->max_result_rect = g_chk_max;
    res->ref_width = g_cur->in.width;
    res->ref_height = g_cur->in.height;
    return PF_Err_NONE;
}

// ----------------------------------------------------------- suite wiring ---
void init_suites() {
    init_ansi();
    g_handle.host_new_handle = H_new;
    g_handle.host_lock_handle = H_lock;
    g_handle.host_unlock_handle = H_unlock;
    g_handle.host_dispose_handle = H_dispose;
    g_handle.host_get_handle_size = H_size;
    g_handle.host_resize_handle = H_resize;
    g_color.PF_GetFloatingPointColorFromColorDef = get_color;
    g_paramutils.PF_UpdateParamUI = update_param_ui;
    g_it8.iterate = iterate8;
    g_it16.iterate = iterate16;
    g_itf.iterate = iteratef;
    g_world.PF_NewWorld = new_world;
    g_world.PF_DisposeWorld = dispose_world;
    g_util3.AEGP_RegisterWithAEGP = fake_register;
    g_util6.AEGP_RegisterWithAEGP = fake_register;
    g_pfi.AEGP_GetEffectLayer = fake_get_effect_layer;
    g_pfi.AEGP_GetNewEffectForEffect = fake_get_new_effect_for_effect;
    g_effect.AEGP_DisposeEffect = fake_dispose_effect;
    g_stream.AEGP_GetNewEffectStreamByIndex = fake_get_stream;
    g_stream.AEGP_DisposeStream = fake_dispose_stream;
    g_dyn.AEGP_SetDynamicStreamFlag = fake_set_dyn_flag;
    g_dyn4.AEGP_SetDynamicStreamFlag = fake_set_dyn_flag;
    g_basic.AcquireSuite = acquire_suite;
    g_basic.ReleaseSuite = release_suite;
}

void init_side(Side &s, bool binary, const char *label) {
    s.binary = binary;
    s.label = label;
    g_sides[binary ? 1 : 0] = &s;
    s.utils.copy = copy_world;
    s.utils.ansi = g_ansi;
    s.in.utils = &s.utils;
    s.in.effect_ref = (PF_ProgPtr)&s;
    s.in.pica_basicP = &g_basic;
    s.in.inter.add_param = add_param;
    s.in.inter.checkout_param = checkout_param;
    s.in.inter.checkin_param = checkin_param;
    s.in.inter.register_ui = register_ui;
    s.in.current_time = 0;
    s.in.time_step = 1;
    s.in.time_scale = 30;
    s.in.downsample_x = {1, 1};
    s.in.downsample_y = {1, 1};
    s.in.global_data = nullptr;
    s.out.num_params = 0;
    AEFX_CLR_STRUCT(s.input_def);
    s.input_def.param_type = PF_Param_LAYER;
    s.ptrs[0] = &s.input_def;
    for (int i = 1; i < kNumParams; ++i) s.ptrs[(size_t)i] = &s.params[(size_t)i];
}


// ==================================================== spec-based checks ====
// Expected values come from the verified binary spec (OLMColorKeep-20261120.md),
// not from running the binary. The binary-differential run lives in the e2e harness.

int g_checks = 0;
#define CHK(cond, ...)             \
    do {                           \
        ++g_checks;                \
        EXPECT(cond, __VA_ARGS__); \
    } while (0)

Side g_s;

PF_Err Run(int cmd, void *params, void *output, void *extra) {
    g_cur = &g_s;
    return EffectMain((PF_Cmd)cmd, &g_s.in, &g_s.out, (PF_ParamDef **)params, (PF_LayerDef *)output, extra);
}

void Boot() {
    g_s = Side{};
    init_side(g_s, false, "port");
    CHK(Run(1, g_s.ptrs.data(), nullptr, nullptr) == PF_Err_NONE, "GLOBAL_SETUP");
    CHK(Run(4, g_s.ptrs.data(), nullptr, nullptr) == PF_Err_NONE, "PARAMS_SETUP");
}

void TestIdentity() {
    CHK(Run(0, nullptr, nullptr, nullptr) == PF_Err_NONE, "ABOUT");
    CHK(std::strcmp(g_s.out.return_msg, "OLM Color Keep 1.1\rConstraint colors") == 0, "about text '%s'",
        g_s.out.return_msg);
    CHK(g_s.out.my_version == 0x00088000, "my_version 0x%x (spec 0x00088000)", (unsigned)g_s.out.my_version);
    CHK(g_s.out.out_flags == 0x06000040, "out_flags 0x%x (binary 0x02000040 + SEND_UPDATE_PARAMS_UI)", (unsigned)g_s.out.out_flags);
    CHK(g_s.out.out_flags2 == 0x08001400, "out_flags2 0x%x (spec 0x08001400)", (unsigned)g_s.out.out_flags2);
    CHK(g_s.aegp_register_names == "OLM Color Keep;", "AEGP name '%s'", g_s.aegp_register_names.c_str());
    CHK(g_s.out.num_params == 202, "num_params %d (spec 202)", (int)g_s.out.num_params);
    CHK(g_s.add_count == 201, "add_param calls %d (spec 201)", g_s.add_count);
    const PF_ParamDef &en = g_s.defaults[1];
    CHK(en.param_type == PF_Param_SLIDER && en.uu.id == 1 && en.flags == PF_ParamFlag_SUPERVISE,
        "Enabled type/id/flags");
    CHK(en.u.sd.value == 1 && en.u.sd.dephault == 1, "Enabled value/dflt");
    CHK(en.u.sd.valid_min == 0 && en.u.sd.valid_max == 200 && en.u.sd.slider_min == 0 && en.u.sd.slider_max == 200,
        "Enabled ranges");
    CHK(std::strcmp(en.PF_DEF_NAME, "Enabled Color Num") == 0, "Enabled name");
    bool colours_ok = true;
    for (int i = 0; i < 200; ++i) {
        const PF_ParamDef &c = g_s.defaults[(size_t)(2 + i)];
        colours_ok &= c.param_type == PF_Param_COLOR && c.uu.id == 2 + i && c.flags == 0 && c.u.cd.value.alpha == 255 &&
                      c.u.cd.value.red == 0 && c.u.cd.value.green == 0 && c.u.cd.value.blue == 0 &&
                      std::strcmp(c.PF_DEF_NAME, "Color") == 0;
    }
    CHK(colours_ok, "colour defs 2..201 (COLOR, flags 0, opaque black, name 'Color')");
}

// Hidden iff index >= count, for every colour; exactly 200 writes.
void TestUI() {
    static const A_long counts[] = {-3, 0, 1, 37, 200};
    for (A_long c : counts) {
        g_s.params[(size_t)OLMCKP_COUNT].u.sd.value = c;
        g_s.trace.clear();
        g_s.stream_state.clear();
        CHK(Run(14, g_s.ptrs.data(), nullptr, nullptr) == PF_Err_NONE, "UPDATE_PARAMS_UI count=%d", (int)c);
        CHK(g_s.trace.size() == 200, "count=%d: %zu writes (spec 200)", (int)c, g_s.trace.size());
        int bad = 0;
        for (int i = 0; i < 200; ++i) {
            auto it = g_s.stream_state.find(2 + i);
            if (it == g_s.stream_state.end() || it->second != (i >= c ? 1 : 0)) ++bad;
        }
        CHK(bad == 0, "count=%d: %d colours with wrong HIDDEN", (int)c, bad);
    }
    // USER_CHANGED_PARAM is a no-op in the binary: no AEGP traffic.
    g_s.trace.clear();
    CHK(Run(13, g_s.ptrs.data(), nullptr, nullptr) == PF_Err_NONE && g_s.trace.empty(), "USER_CHANGED no-op");
    g_s.params[(size_t)OLMCKP_COUNT].u.sd.value = 1;
}

void TestPreRender() {
    auto pre = [](PF_LRect res, PF_LRect mx, PF_LRect outr, PF_LRect outm, PF_LRect *ro, PF_LRect *rm) {
        PF_PreRenderInput pin{};
        PF_PreRenderOutput pout{};
        pout.result_rect = outr;
        pout.max_result_rect = outm;
        PF_PreRenderCallbacks pcb{};
        pcb.checkout_layer = pre_checkout;
        PF_PreRenderExtra pe{&pin, &pout, &pcb};
        g_chk_result = res;
        g_chk_max = mx;
        g_s.pre_calls = 0;
        const PF_Err err = Run(23, nullptr, nullptr, &pe);
        CHK(err == PF_Err_NONE && g_s.pre_calls == 1 && g_s.pre_index == 0, "PRE checkout of input layer");
        *ro = pout.result_rect;
        *rm = pout.max_result_rect;
    };
    auto eq = [](PF_LRect a, PF_LRect b) {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    };
    PF_LRect ro, rm;
    const PF_LRect E{0, 0, 0, 0}, A{1, 2, 5, 6}, B{3, 0, 9, 4};
    pre(A, B, E, E, &ro, &rm);
    CHK(eq(ro, A) && eq(rm, B), "empty output takes the result rects");
    pre(A, B, B, A, &ro, &rm);
    CHK(eq(ro, PF_LRect{1, 0, 9, 6}) && eq(rm, PF_LRect{1, 0, 9, 6}), "non-empty output unions");
    pre(E, E, A, B, &ro, &rm);
    CHK(eq(ro, A) && eq(rm, B), "empty result leaves output unchanged");
}

// Single-pixel SmartRender at a given depth and colour (black by default).
struct Px {
    float a, r, g, b;
};

// Runs one pixel through SmartRender with count `count` and colour 0 = `colour`.
void RenderOne(int depth, A_long count, const PF_Pixel8 &colour, Px in, Px *out_px) {
    g_s.params[(size_t)OLMCKP_COUNT].u.sd.value = count;
    g_s.params[(size_t)OLMCKP_COLOR_BASE].u.cd.value = colour;
    g_s.params[(size_t)OLMCKP_COLOR_BASE].u.cd.dephault = colour;
    g_s.pixsize = depth == 8 ? 4 : depth == 16 ? 8 : 16;
    unsigned char ibuf[16] = {0}, obuf[16] = {0};
    if (depth == 8) {
        ibuf[0] = (unsigned char)in.a; ibuf[1] = (unsigned char)in.r; ibuf[2] = (unsigned char)in.g; ibuf[3] = (unsigned char)in.b;
        std::memset(obuf, 0x5A, 4);
    } else if (depth == 16) {
        uint16_t v[4] = {(uint16_t)in.a, (uint16_t)in.r, (uint16_t)in.g, (uint16_t)in.b};
        std::memcpy(ibuf, v, 8);
        std::memset(obuf, 0x5A, 8);
    } else {
        float v[4] = {in.a, in.r, in.g, in.b};
        std::memcpy(ibuf, v, 16);
        std::memset(obuf, 0x5A, 16);
    }
    AEFX_CLR_STRUCT(g_s.in_world);
    g_s.in_world.data = (PF_PixelPtr)ibuf;
    g_s.in_world.rowbytes = g_s.pixsize;
    g_s.in_world.width = 1;
    g_s.in_world.height = 1;
    g_s.in_world.extent_hint = {0, 0, 1, 1};
    AEFX_CLR_STRUCT(g_s.out_world);
    g_s.out_world.data = (PF_PixelPtr)obuf;
    g_s.out_world.rowbytes = g_s.pixsize;
    g_s.out_world.width = 1;
    g_s.out_world.height = 1;
    g_s.out_world.extent_hint = {0, 0, 1, 1};
    PF_SmartRenderInput sin{};
    sin.bitdepth = (short)depth;
    PF_SmartRenderCallbacks rcb{};
    rcb.checkout_layer_pixels = checkout_layer_pixels;
    rcb.checkin_layer_pixels = checkin_layer_pixels;
    rcb.checkout_output = checkout_output;
    PF_SmartRenderExtra sx{&sin, &rcb};
    const PF_Err err = Run(24, g_s.ptrs.data(), nullptr, &sx);
    CHK(err == PF_Err_NONE, "SMART_RENDER depth %d err %d", depth, (int)err);
    if (depth == 8) {
        out_px->a = obuf[0]; out_px->r = obuf[1]; out_px->g = obuf[2]; out_px->b = obuf[3];
    } else if (depth == 16) {
        uint16_t v[4];
        std::memcpy(v, obuf, 8);
        out_px->a = v[0]; out_px->r = v[1]; out_px->g = v[2]; out_px->b = v[3];
    } else {
        float v[4];
        std::memcpy(v, obuf, 16);
        out_px->a = v[0]; out_px->r = v[1]; out_px->g = v[2]; out_px->b = v[3];
    }
}

void TestRender() {
    const PF_Pixel8 black{255, 0, 0, 0};
    Px o{};
    // 8 bpc: exact black is kept (alpha kept, RGB kept); 1/255 off-black is not.
    RenderOne(8, 1, black, Px{200, 0, 0, 0}, &o);
    CHK(o.a == 200 && o.r == 0 && o.g == 0 && o.b == 0, "8bpc black kept");
    RenderOne(8, 1, black, Px{200, 1, 0, 0}, &o);
    CHK(o.a == 0 && o.r == 1 && o.g == 0 && o.b == 0, "8bpc 1/255 red not kept, RGB preserved");
    // Colour alpha is ignored; the pixel alpha is what survives.
    RenderOne(8, 1, PF_Pixel8{0, 0, 0, 0}, Px{37, 0, 0, 0}, &o);
    CHK(o.a == 37, "8bpc colour alpha ignored (kept alpha 37), got %d", (int)o.a);
    // count 0: nothing kept.
    RenderOne(8, 0, black, Px{200, 0, 0, 0}, &o);
    CHK(o.a == 0 && o.r == 0, "8bpc count 0 drops all");
    // count < 0 behaves like 0.
    RenderOne(8, -3, black, Px{200, 0, 0, 0}, &o);
    CHK(o.a == 0, "8bpc count -3 drops all");
    // 16 bpc: 3/32768 (9.2e-5) is within 1e-4 of black, 4/32768 (1.22e-4) is not.
    RenderOne(16, 1, black, Px{32768, 3, 0, 0}, &o);
    CHK(o.a == 32768 && o.r == 3, "16bpc 3/32768 kept (within 1e-4)");
    RenderOne(16, 1, black, Px{32768, 4, 0, 0}, &o);
    CHK(o.a == 0 && o.r == 4, "16bpc 4/32768 not kept");
    // 32 bpc: 9e-5 kept, 1.1e-4 not. NaN compares as a match (binary).
    RenderOne(32, 1, black, Px{0.5f, 9e-5f, 0, 0}, &o);
    CHK(o.a == 0.5f && std::fabs(o.r - 9e-5f) < 1e-12f, "32bpc 9e-5 kept");
    RenderOne(32, 1, black, Px{0.5f, 1.1e-4f, 0, 0}, &o);
    CHK(o.a == 0.0f, "32bpc 1.1e-4 not kept");
    RenderOne(32, 1, black, Px{0.5f, std::numeric_limits<float>::quiet_NaN(), 0, 0}, &o);
    CHK(o.a == 0.5f, "32bpc NaN channel counts as match");
    // First match wins and any colour in 0..count-1 matches.
    g_s.params[(size_t)OLMCKP_COLOR_BASE + 1].u.cd.value = PF_Pixel8{255, 255, 0, 0};
    g_s.params[(size_t)OLMCKP_COLOR_BASE + 1].u.cd.dephault = PF_Pixel8{255, 255, 0, 0};
    RenderOne(8, 2, PF_Pixel8{255, 0, 0, 0}, Px{90, 255, 0, 0}, &o);
    CHK(o.a == 90, "8bpc colour index 1 (red) matches when count 2");
    RenderOne(8, 1, PF_Pixel8{255, 0, 0, 0}, Px{90, 255, 0, 0}, &o);
    CHK(o.a == 0, "8bpc colour index 1 ignored when count 1");
    g_s.params[(size_t)OLMCKP_COLOR_BASE + 1].u.cd.value = PF_Pixel8{255, 0, 0, 0};
    g_s.params[(size_t)OLMCKP_COLOR_BASE + 1].u.cd.dephault = PF_Pixel8{255, 0, 0, 0};
}

void TestCheckoutBalance() {
    // Every checkout_layer_pixels / checkout_param is matched by a checkin.
    CHK(g_s.layer_checkouts == g_s.layer_checkins, "layer checkouts %d vs checkins %d", g_s.layer_checkouts,
        g_s.layer_checkins);
    CHK(g_s.checkout_calls == g_s.checkin_calls, "param checkouts %d vs checkins %d", g_s.checkout_calls,
        g_s.checkin_calls);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    init_suites();
    Boot();
    TestIdentity();
    TestUI();
    TestPreRender();
    TestRender();
    Run(3, g_s.ptrs.data(), nullptr, nullptr);
    TestCheckoutBalance();
    std::printf("test_color_keep: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

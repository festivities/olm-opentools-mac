// Fake AE host; production EffectMain runs GLOBAL_SETUP -> PARAMS_SETUP ->
// UPDATE_PARAMS_UI / USER_CHANGED_PARAM -> SMART_PRE_RENDER -> SMART_RENDER.
// Covers: 102-param identities/defaults, Color stream hiding by count, exact
// ARGB match (alpha included) with rounding quantization at 8/16 bpc and the
// 1e-4 tolerance at 32 bpc, count gating, RGB preservation, extent-limited
// iteration over a utils->copy pass-through, and layer checkout balance.
#include "OLMColorKeep.cpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr A_long kW = 4, kH = 3;
int g_checks = 0, g_failures = 0;

#define CHECK(cond, msg)                              \
    do {                                              \
        ++g_checks;                                   \
        if (!(cond)) {                                \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++g_failures;                             \
        }                                             \
    } while (0)

struct Host {
    int bitdepth = 8;
    SPBasicSuite basic{};
    PF_UtilCallbacks utils{};
    PF_InData in{};
    PF_OutData out{};
    std::vector<unsigned char> in_bytes, out_bytes;
    PF_EffectWorld input{}, output{};
    PF_ParamDef params[OLMCKP_NUM_PARAMS]{};
    PF_ParamDef *param_ptrs[OLMCKP_NUM_PARAMS]{};
    int add_count = 0, layer_checkouts = 0, layer_checkins = 0;
    int param_checkouts = 0, param_checkins = 0;
    std::vector<std::pair<PF_ParamIndex, A_Boolean>> hidden;
    int stream_disposes = 0, effect_disposes = 0;

    PF_ColorParamSuite1 color_suite{};
    PF_Iterate8Suite1 it8{};
    PF_Iterate16Suite1 it16{};
    PF_IterateFloatSuite1 itf{};
    AEGP_UtilitySuite3 util{};
    AEGP_PFInterfaceSuite1 pfi{};
    AEGP_EffectSuite5 effect{};
    AEGP_StreamSuite7 stream{};
    AEGP_DynamicStreamSuite3 dyn{};
};
Host *g = nullptr;

size_t PixSize() { return g->bitdepth == 8 ? 4 : g->bitdepth == 16 ? 8 : 16; }

template <typename P>
P *At(PF_EffectWorld &w, A_long x, A_long y) {
    return (P *)((char *)w.data + y * w.rowbytes + x * (A_long)sizeof(P));
}

PF_Err add_param(PF_ProgPtr, PF_ParamIndex, PF_ParamDefPtr def) {
    if (g->add_count >= OLMCKP_NUM_PARAMS - 1) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    g->params[++g->add_count] = *def;
    return PF_Err_NONE;
}
PF_Err checkout_param(PF_ProgPtr, PF_ParamIndex i, A_long, A_long, A_u_long, PF_ParamDef *p) {
    if (i <= 0 || i >= OLMCKP_NUM_PARAMS) return PF_Err_BAD_CALLBACK_PARAM;
    ++g->param_checkouts;
    *p = g->params[i];
    return PF_Err_NONE;
}
PF_Err checkin_param(PF_ProgPtr, PF_ParamDef *) { ++g->param_checkins; return PF_Err_NONE; }
PF_Err copy_world(PF_ProgPtr, PF_EffectWorld *s, PF_EffectWorld *d, PF_Rect *, PF_Rect *) {
    std::memcpy(d->data, s->data, (size_t)s->rowbytes * (size_t)s->height);
    return PF_Err_NONE;
}
int SPAPI mock_sprintf(A_char *buf, const A_char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    const int n = std::vsnprintf(buf, 256, fmt, a);
    va_end(a);
    return n;
}
// Key colors are delivered as float; tests set them via the 8-bit def value
// plus an optional float override (to probe 16/32-bpc boundaries).
PF_PixelFloat g_override[OLMCKP_MAX_COLORS];
bool g_use_override[OLMCKP_MAX_COLORS];
PF_Err get_color(PF_ProgPtr, const PF_ParamDef *def, PF_PixelFloat *o) {
    const int slot = def->uu.id - OLMCKP_COLOR_BASE;
    if (slot >= 0 && slot < OLMCKP_MAX_COLORS && g_use_override[slot]) {
        *o = g_override[slot];
        return PF_Err_NONE;
    }
    o->alpha = def->u.cd.value.alpha / 255.0f;
    o->red = def->u.cd.value.red / 255.0f;
    o->green = def->u.cd.value.green / 255.0f;
    o->blue = def->u.cd.value.blue / 255.0f;
    return PF_Err_NONE;
}
template <typename P>
PF_Err iterate(PF_InData *, A_long, A_long, PF_EffectWorld *s, const PF_Rect *r, void *rc,
               PF_Err (*fn)(void *, A_long, A_long, P *, P *), PF_EffectWorld *d) {
    for (A_long y = r->top; y < r->bottom; ++y)
        for (A_long x = r->left; x < r->right; ++x) fn(rc, x, y, At<P>(*s, x, y), At<P>(*d, x, y));
    return PF_Err_NONE;
}
PF_Err reg(AEGP_GlobalRefcon, const A_char *, AEGP_PluginID *id) { *id = 7; return A_Err_NONE; }
PF_Err get_layer(PF_ProgPtr, AEGP_LayerH *l) { *l = (AEGP_LayerH)1; return A_Err_NONE; }
PF_Err get_effect(AEGP_PluginID, PF_ProgPtr, AEGP_EffectRefH *e) { *e = (AEGP_EffectRefH)2; return A_Err_NONE; }
PF_Err dispose_effect(AEGP_EffectRefH) { ++g->effect_disposes; return A_Err_NONE; }
PF_Err get_stream(AEGP_PluginID, AEGP_EffectRefH, PF_ParamIndex i, AEGP_StreamRefH *s) {
    *s = (AEGP_StreamRefH)(ptrdiff_t)(i + 1);
    return A_Err_NONE;
}
PF_Err dispose_stream(AEGP_StreamRefH) { ++g->stream_disposes; return A_Err_NONE; }
PF_Err set_flag(AEGP_StreamRefH s, AEGP_DynStreamFlags f, A_Boolean undoable, A_Boolean set) {
    if (f != AEGP_DynStreamFlag_HIDDEN || undoable) return (A_Err)1;
    g->hidden.push_back({(PF_ParamIndex)((ptrdiff_t)s - 1), set});
    return A_Err_NONE;
}
SPErr SPAPI acquire(const char *n, int32 v, const void **s) {
    struct { const char *n; int32 v; const void *s; } t[] = {
        {kPFColorParamSuite, kPFColorParamSuiteVersion1, &g->color_suite},
        {kPFIterate8Suite, kPFIterate8SuiteVersion1, &g->it8},
        {kPFIterate16Suite, kPFIterate16SuiteVersion1, &g->it16},
        {kPFIterateFloatSuite, kPFIterateFloatSuiteVersion1, &g->itf},
        {kAEGPUtilitySuite, kAEGPUtilitySuiteVersion3, &g->util},
        {kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1, &g->pfi},
        {kAEGPEffectSuite, kAEGPEffectSuiteVersion5, &g->effect},
        {kAEGPStreamSuite, kAEGPStreamSuiteVersion7, &g->stream},
        {kAEGPDynamicStreamSuite, kAEGPDynamicStreamSuiteVersion3, &g->dyn}};
    for (auto &e : t)
        if (!std::strcmp(n, e.n) && v == e.v) { *s = e.s; return kSPNoError; }
    return (SPErr)-1;
}
SPErr SPAPI release(const char *, int32) { return kSPNoError; }
PF_Err co_layer(PF_ProgPtr, A_long, PF_EffectWorld **w) { ++g->layer_checkouts; *w = &g->input; return PF_Err_NONE; }
PF_Err ci_layer(PF_ProgPtr, A_long) { ++g->layer_checkins; return PF_Err_NONE; }
PF_Err co_output(PF_ProgPtr, PF_EffectWorld **w) { *w = &g->output; return PF_Err_NONE; }
PF_Err pre_checkout(PF_ProgPtr, PF_ParamIndex, A_long, const PF_RenderRequest *r, A_long, A_long,
                    A_u_long, PF_CheckoutResult *res) {
    res->result_rect = res->max_result_rect = r->rect;
    return PF_Err_NONE;
}

void SetWorld(PF_EffectWorld &w, std::vector<unsigned char> &b) {
    b.assign((size_t)kH * (kW * PixSize() + 16), 0);  // padded rows
    AEFX_CLR_STRUCT(w);
    w.data = (PF_PixelPtr)b.data();
    w.rowbytes = (A_long)(kW * PixSize() + 16);
    w.width = kW;
    w.height = kH;
    w.extent_hint = {0, 0, kW, kH};
}

void Init(Host &h, int depth) {
    g = &h;
    h.bitdepth = depth;
    std::memset(g_use_override, 0, sizeof(g_use_override));
    SetWorld(h.input, h.in_bytes);
    SetWorld(h.output, h.out_bytes);
    h.color_suite.PF_GetFloatingPointColorFromColorDef = get_color;
    h.it8.iterate = iterate<PF_Pixel>;
    h.it16.iterate = iterate<PF_Pixel16>;
    h.itf.iterate = iterate<PF_PixelFloat>;
    h.util.AEGP_RegisterWithAEGP = reg;
    h.pfi.AEGP_GetEffectLayer = get_layer;
    h.pfi.AEGP_GetNewEffectForEffect = get_effect;
    h.effect.AEGP_DisposeEffect = dispose_effect;
    h.stream.AEGP_GetNewEffectStreamByIndex = get_stream;
    h.stream.AEGP_DisposeStream = dispose_stream;
    h.dyn.AEGP_SetDynamicStreamFlag = set_flag;
    h.basic.AcquireSuite = acquire;
    h.basic.ReleaseSuite = release;
    h.utils.copy = copy_world;
    h.utils.ansi.sprintf = mock_sprintf;
    h.in.inter.add_param = add_param;
    h.in.inter.checkout_param = checkout_param;
    h.in.inter.checkin_param = checkin_param;
    h.in.utils = &h.utils;
    h.in.effect_ref = (PF_ProgPtr)&h;
    h.in.pica_basicP = &h.basic;
    h.in.time_step = 1;
    h.in.time_scale = 30;
    for (int i = 0; i < OLMCKP_NUM_PARAMS; ++i) h.param_ptrs[i] = &h.params[i];
    EffectMain(PF_Cmd_GLOBAL_SETUP, &h.in, &h.out, nullptr, nullptr, nullptr);
    EffectMain(PF_Cmd_PARAMS_SETUP, &h.in, &h.out, nullptr, nullptr, nullptr);
}

PF_Err Render(Host &h, PF_Rect extent = {0, 0, kW, kH}) {
    h.output.extent_hint = extent;
    PF_PreRenderInput pin{};
    PF_PreRenderOutput pout{};
    PF_PreRenderCallbacks pcb{};
    PF_PreRenderExtra pex{};
    pin.output_request.rect = extent;
    pcb.checkout_layer = pre_checkout;
    pex.input = &pin;
    pex.output = &pout;
    pex.cb = &pcb;
    PF_Err err = EffectMain(PF_Cmd_SMART_PRE_RENDER, &h.in, &h.out, nullptr, nullptr, &pex);
    CHECK(!err && pout.result_rect.right == extent.right, "pre-render unions checkout rect");
    PF_SmartRenderInput rin{};
    PF_SmartRenderCallbacks rcb{};
    PF_SmartRenderExtra rex{};
    rin.bitdepth = (short)h.bitdepth;
    rcb.checkout_layer_pixels = co_layer;
    rcb.checkin_layer_pixels = ci_layer;
    rcb.checkout_output = co_output;
    rex.input = &rin;
    rex.cb = &rcb;
    return err ? err : EffectMain(PF_Cmd_SMART_RENDER, &h.in, &h.out, nullptr, nullptr, &rex);
}

void SetKey(Host &h, int slot, A_u_char r, A_u_char gr, A_u_char b) {
    PF_Pixel &v = h.params[OLMCKP_COLOR_BASE + slot].u.cd.value;
    v.red = r; v.green = gr; v.blue = b; v.alpha = 255;
}

// Writes channel values given in the depth's native integer scale (float: /255).
template <typename P> void Put(Host &h, A_long x, A_long y, double a, double r, double gr, double b);
template <> void Put<PF_Pixel>(Host &h, A_long x, A_long y, double a, double r, double gr, double b) {
    *At<PF_Pixel>(h.input, x, y) = {(A_u_char)a, (A_u_char)r, (A_u_char)gr, (A_u_char)b};
}
template <> void Put<PF_Pixel16>(Host &h, A_long x, A_long y, double a, double r, double gr, double b) {
    *At<PF_Pixel16>(h.input, x, y) = {(A_u_short)a, (A_u_short)r, (A_u_short)gr, (A_u_short)b};
}
template <> void Put<PF_PixelFloat>(Host &h, A_long x, A_long y, double a, double r, double gr, double b) {
    *At<PF_PixelFloat>(h.input, x, y) = {(float)a, (float)r, (float)gr, (float)b};
}

template <typename P>
void RunDepth(int depth, double maxv) {
    Host h;
    Init(h, depth);
    const bool is_float = depth == 32;
    auto scale = [&](int v8) { return is_float ? v8 / 255.0 : depth == 8 ? v8 : (double)(int)((v8 / 255.0f + 1.0f / 65536) * 32768.0f); };

    // Fill: (0,0) red opaque, (1,0) red half alpha, (2,0) green opaque,
    // (3,0) blue opaque, row 1 = slot-3 color, row 2 = gray.
    for (A_long x = 0; x < kW; ++x) {
        Put<P>(h, x, 1, maxv, scale(10), scale(20), scale(30));
        Put<P>(h, x, 2, maxv, scale(128), scale(128), scale(128));
    }
    Put<P>(h, 0, 0, maxv, scale(255), 0, 0);
    Put<P>(h, 1, 0, is_float ? 0.5 : maxv / 2, scale(255), 0, 0);
    Put<P>(h, 2, 0, maxv, 0, scale(255), 0);
    Put<P>(h, 3, 0, maxv, 0, 0, scale(255));

    SetKey(h, 0, 255, 0, 0);
    SetKey(h, 1, 0, 255, 0);
    SetKey(h, 3, 10, 20, 30);  // slot 3 (4th color), gated by count
    h.params[OLMCKP_COUNT].u.sd.value = 2;
    CHECK(Render(h) == PF_Err_NONE, "render ok");
    auto out = [&](A_long x, A_long y) { return *At<P>(h.output, x, y); };
    auto in = [&](A_long x, A_long y) { return *At<P>(h.input, x, y); };
    CHECK(out(0, 0).alpha == in(0, 0).alpha, "opaque red kept");
    CHECK(out(1, 0).alpha == 0, "semi-transparent red dropped (alpha is matched too)");
    CHECK(out(1, 0).red == in(1, 0).red, "dropped pixel keeps RGB");
    CHECK(out(2, 0).alpha == in(2, 0).alpha, "second color kept");
    CHECK(out(3, 0).alpha == 0 && out(3, 0).blue == in(3, 0).blue, "unkeyed blue dropped, RGB kept");
    CHECK(out(0, 1).alpha == 0, "slot 3 inactive at count 2");
    CHECK(out(0, 2).alpha == 0 && out(0, 2).green == in(0, 2).green, "gray dropped");

    h.params[OLMCKP_COUNT].u.sd.value = 4;
    Render(h);
    CHECK(out(0, 1).alpha == in(0, 1).alpha, "slot 3 active at count 4");

    h.params[OLMCKP_COUNT].u.sd.value = 0;
    Render(h);
    CHECK(out(0, 0).alpha == 0 && out(2, 0).alpha == 0, "count 0 drops all");
    h.params[OLMCKP_COUNT].u.sd.value = 1000;  // guarded, no OOB
    CHECK(Render(h) == PF_Err_NONE && out(0, 0).alpha == in(0, 0).alpha, "oversized count clamps");

    // Extent: only column 0 is iterated; others keep the utils->copy pass-through.
    h.params[OLMCKP_COUNT].u.sd.value = 1;
    Render(h, {0, 0, 1, kH});
    CHECK(out(0, 2).alpha == 0, "inside extent processed");
    CHECK(out(3, 2).alpha == in(3, 2).alpha, "outside extent passes through");

    // Quantization / tolerance boundary on the red channel of slot 0.
    g_use_override[0] = true;
    g_override[0] = {1.0f, 0.5f, 0.0f, 0.0f};
    if (depth == 8) {
        // (int)((0.5 + 1/510) * 255) = 128
        Put<P>(h, 0, 2, 255, 128, 0, 0); Put<P>(h, 1, 2, 255, 127, 0, 0);
    } else if (depth == 16) {
        // (int)((0.5 + 1/65536) * 32768) = 16384
        Put<P>(h, 0, 2, 32768, 16384, 0, 0); Put<P>(h, 1, 2, 32768, 16385, 0, 0);
    } else {
        Put<P>(h, 0, 2, 1.0, 0.50009, 0, 0); Put<P>(h, 1, 2, 1.0, 0.5002, 0, 0);
    }
    Render(h);
    CHECK(out(0, 2).alpha == in(0, 2).alpha, "boundary value matches");
    CHECK(out(1, 2).alpha == 0, "one step off rejects");
    if (is_float) {
        Put<P>(h, 2, 2, 1.0, std::nanf(""), 0, 0);
        Render(h);
        CHECK(out(2, 2).alpha == 1.0f, "float NaN channel matches (comiss/ja quirk)");
    }

    CHECK(h.layer_checkouts == h.layer_checkins && h.layer_checkouts > 0, "layer checkout balanced");
    CHECK(h.param_checkouts == h.param_checkins, "param checkout balanced");
}

void TestSetupAndUi() {
    Host h;
    Init(h, 8);
    CHECK(h.out.num_params == 102 && h.add_count == 101, "102 params");
    CHECK(h.out.my_version == 526336, "version 1.0.1");
    CHECK(h.out.out_flags == 0x06000040 && h.out.out_flags2 == 0x08001400, "flags");
    const PF_ParamDef &c = h.params[1];
    CHECK(c.uu.id == 1 && c.param_type == PF_Param_SLIDER && c.flags == PF_ParamFlag_SUPERVISE,
          "count slider identity");
    CHECK(!std::strcmp(c.PF_DEF_NAME, "Enabled Color Num"), "count name");
    CHECK(c.u.sd.valid_min == 0 && c.u.sd.valid_max == 100 && c.u.sd.slider_max == 100 &&
              c.u.sd.dephault == 1 && c.u.sd.value == 1, "count ranges");
    bool colors_ok = true;
    for (int i = 0; i < 100; ++i) {
        const PF_ParamDef &d = h.params[2 + i];
        colors_ok &= d.uu.id == 2 + i && d.param_type == PF_Param_COLOR &&
                     !std::strcmp(d.PF_DEF_NAME, "Color") && d.u.cd.value.alpha == 255 &&
                     d.u.cd.value.red == 0 && d.u.cd.dephault.alpha == 255 && d.flags == 0;
    }
    CHECK(colors_ok, "100 black Color params, ids 2..101");

    EffectMain(PF_Cmd_ABOUT, &h.in, &h.out, nullptr, nullptr, nullptr);
    CHECK(!std::strcmp(h.out.return_msg, OLMCKP_ABOUT), "about text");

    const A_long counts[] = {0, 1, 37, 100};
    const PF_Cmd cmds[] = {PF_Cmd_UPDATE_PARAMS_UI, PF_Cmd_USER_CHANGED_PARAM};
    for (PF_Cmd cmd : cmds) {
        for (A_long n : counts) {
            h.hidden.clear();
            h.params[1].u.sd.value = n;
            EffectMain(cmd, &h.in, &h.out, h.param_ptrs, nullptr, nullptr);
            bool ok = h.hidden.size() == 100;
            for (size_t i = 0; ok && i < 100; ++i)
                ok = h.hidden[i].first == (PF_ParamIndex)(2 + i) &&
                     h.hidden[i].second == ((A_long)i >= n ? TRUE : FALSE);
            CHECK(ok, "Color n hidden iff n >= count");
        }
    }
    CHECK(h.stream_disposes == 800 && h.effect_disposes == 8, "AEGP handles disposed");
}

}  // namespace

int main() {
    TestSetupAndUi();
    RunDepth<PF_Pixel>(8, 255);
    RunDepth<PF_Pixel16>(16, 32768);
    RunDepth<PF_PixelFloat>(32, 1.0);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

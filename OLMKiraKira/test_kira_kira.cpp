// Fake host. Production EffectMain through SMART_RENDER. Checks identities,
// zero-length copy, a vertical streak, and ramp flatten round-trip.
#include "OLMKiraKira.cpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

int g_checks = 0, g_failures = 0;
#define CHECK(cond, msg)                                        \
    do {                                                        \
        ++g_checks;                                             \
        if (!(cond)) {                                          \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

struct Host {
    PF_UtilCallbacks utils{};
    PF_InData in{};
    PF_OutData out{};
    std::vector<unsigned char> in_bytes, out_bytes;
    PF_EffectWorld input{}, output{};
    PF_ParamDef params[OLMKK_NUM_PARAMS]{};
    int add_count = 0, w = 0, h = 0;
    PF_Rect last_request{};
    A_short pre_flags = 0;
    std::vector<PF_Handle> handles;
    PF_ParamUtilsSuite3 param_utils{};
    AEGP_UtilitySuite3 util{};
    SPBasicSuite basic{};
    std::vector<std::pair<int, bool>> disabled;
};

Host *g = nullptr;

PF_Handle new_handle(A_u_longlong size) {
    auto *b = (unsigned char *)std::malloc(sizeof(void *) + size);
    if (!b) return nullptr;
    *(void **)b = b;
    g->handles.push_back((PF_Handle)(b + sizeof(void *)));
    return g->handles.back();
}
void *lock_handle(PF_Handle h) { return h; }
void unlock_handle(PF_Handle) {}
void dispose_handle(PF_Handle h) { if (h) std::free((char *)h - sizeof(void *)); }

PF_Err add_param(PF_ProgPtr, PF_ParamIndex, PF_ParamDefPtr def) {
    if (g->add_count >= OLMKK_NUM_PARAMS - 1) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    g->params[++g->add_count] = *def;
    return PF_Err_NONE;
}
PF_Err checkout_param(PF_ProgPtr, PF_ParamIndex i, A_long, A_long, A_u_long, PF_ParamDef *p) {
    if (i <= 0 || i >= OLMKK_NUM_PARAMS) return PF_Err_BAD_CALLBACK_PARAM;
    *p = g->params[i];
    return PF_Err_NONE;
}
PF_Err checkin_param(PF_ProgPtr, PF_ParamDef *) { return PF_Err_NONE; }
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
PF_Err co_layer(PF_ProgPtr, A_long, PF_EffectWorld **w) { *w = &g->input; return PF_Err_NONE; }
PF_Err co_output(PF_ProgPtr, PF_EffectWorld **w) { *w = &g->output; return PF_Err_NONE; }
PF_Err pre_checkout(PF_ProgPtr, PF_ParamIndex, A_long, const PF_RenderRequest *r, A_long, A_long,
                    A_u_long, PF_CheckoutResult *res) {
    g->last_request = r->rect;
    res->result_rect = res->max_result_rect = r->rect;
    return PF_Err_NONE;
}
PF_Err update_ui(PF_ProgPtr, PF_ParamIndex i, const PF_ParamDef *d) {
    g->disabled.push_back({(int)i, (d->ui_flags & PF_PUI_DISABLED) != 0});
    return PF_Err_NONE;
}
PF_Err reg(AEGP_GlobalRefcon, const A_char *, AEGP_PluginID *id) {
    *id = 9;
    return A_Err_NONE;
}
SPErr SPAPI acquire(const char *n, int32 v, const void **s) {
    if (!std::strcmp(n, kPFParamUtilsSuite) && v == kPFParamUtilsSuiteVersion3) {
        *s = &g->param_utils;
        return kSPNoError;
    }
    if (!std::strcmp(n, kAEGPUtilitySuite) && v == kAEGPUtilitySuiteVersion3) {
        *s = &g->util;
        return kSPNoError;
    }
    return (SPErr)-1;
}
SPErr SPAPI release(const char *, int32) { return kSPNoError; }

void SetWorld(PF_EffectWorld &w, std::vector<unsigned char> &b, int width, int height) {
    const A_long rb = width * 4 + 16;
    b.assign((size_t)rb * height, 0x5A);
    AEFX_CLR_STRUCT(w);
    w.data = (PF_PixelPtr)b.data();
    w.rowbytes = rb;
    w.width = width;
    w.height = height;
}

void Init(Host &host, int w, int height) {
    g = &host;
    host.w = w;
    host.h = height;
    SetWorld(host.input, host.in_bytes, w, height);
    SetWorld(host.output, host.out_bytes, w, height);
    host.utils.ansi.sprintf = mock_sprintf;
    host.utils.copy = copy_world;
    host.utils.host_new_handle = new_handle;
    host.utils.host_lock_handle = lock_handle;
    host.utils.host_unlock_handle = unlock_handle;
    host.utils.host_dispose_handle = dispose_handle;
    host.param_utils.PF_UpdateParamUI = update_ui;
    host.util.AEGP_RegisterWithAEGP = reg;
    host.basic.AcquireSuite = acquire;
    host.basic.ReleaseSuite = release;
    host.in.inter.add_param = add_param;
    host.in.inter.checkout_param = checkout_param;
    host.in.inter.checkin_param = checkin_param;
    host.in.utils = &host.utils;
    host.in.effect_ref = (PF_ProgPtr)&host;
    host.in.pica_basicP = &host.basic;
    host.in.time_step = 1;
    host.in.time_scale = 30;
    host.in.downsample_x = {1, 1};
    host.in.downsample_y = {1, 1};
    host.add_count = 0;
    EffectMain(PF_Cmd_GLOBAL_SETUP, &host.in, &host.out, nullptr, nullptr, nullptr);
    EffectMain(PF_Cmd_PARAMS_SETUP, &host.in, &host.out, nullptr, nullptr, nullptr);
}

void Put(Host &h, int x, int y, PF_Pixel p) {
    ((PF_Pixel *)((char *)h.input.data + (size_t)y * h.input.rowbytes))[x] = p;
}
PF_Pixel Get(Host &h, int x, int y) {
    return ((PF_Pixel *)((char *)h.output.data + (size_t)y * h.output.rowbytes))[x];
}

PF_Err Render(Host &h) {
    PF_PreRenderInput pin{};
    PF_PreRenderOutput pout{};
    PF_PreRenderCallbacks pcb{};
    PF_PreRenderExtra pex{};
    pin.output_request.rect = {1, 2, 3, 4};
    pcb.checkout_layer = pre_checkout;
    pex.input = &pin;
    pex.output = &pout;
    pex.cb = &pcb;
    PF_Err err = EffectMain(PF_Cmd_SMART_PRE_RENDER, &h.in, &h.out, nullptr, nullptr, &pex);
    h.pre_flags = pout.flags;
    PF_SmartRenderInput rin{};
    PF_SmartRenderCallbacks rcb{};
    PF_SmartRenderExtra rex{};
    rin.bitdepth = 8;
    rcb.checkout_layer_pixels = co_layer;
    rcb.checkout_output = co_output;
    rex.input = &rin;
    rex.cb = &rcb;
    return err ? err : EffectMain(PF_Cmd_SMART_RENDER, &h.in, &h.out, nullptr, nullptr, &rex);
}

void ZeroLengths(Host &h) {
    h.params[OLMKK_V_LEN].u.sd.value = 0;
    h.params[OLMKK_H_LEN].u.sd.value = 0;
    h.params[OLMKK_D_LEN].u.sd.value = 0;
    h.params[OLMKK_D2_LEN].u.sd.value = 0;
    h.params[OLMKK_HL_RADIUS].u.sd.value = 0;
}

void TestIdentity() {
    Host h;
    Init(h, 4, 4);
    CHECK(h.out.my_version == 0x00198000, "version");
    CHECK(h.out.out_flags == 0x06008040, "flags");
    CHECK(h.out.out_flags2 == 0x08001400, "flags2");
    CHECK(h.out.num_params == 41, "count");
    CHECK(h.params[OLMKK_CHANNEL].uu.id == 8 && h.params[OLMKK_CHANNEL].u.pd.value == 1, "channel");
    CHECK(h.params[OLMKK_BLUR_MODE].uu.id == 9 && h.params[OLMKK_BLUR_MODE].u.pd.value == 2, "blur");
    CHECK(h.params[OLMKK_MERGE].uu.id == 17 && h.params[OLMKK_MERGE].u.pd.num_choices == 2, "merge");
    CHECK(h.params[OLMKK_GAIN].uu.id == 2 && h.params[OLMKK_GAIN].u.fs_d.value == 0.1f, "gain");
    CHECK(h.params[OLMKK_V_LEN].uu.id == 3 && h.params[OLMKK_V_LEN].u.sd.valid_max == 1000, "vlen");
    CHECK(h.params[OLMKK_V_LEN].u.sd.slider_max == 200, "v slider");
    CHECK(h.params[OLMKK_ROTATION].uu.id == 1, "rotation id");
    CHECK(std::strcmp(h.params[OLMKK_CHANNEL].PF_DEF_NAME, "Channel") == 0, "channel name");
    CHECK(h.params[OLMKK_V_RAMP].u.arb_d.dephault != nullptr, "default ramp");
    EffectMain(PF_Cmd_ABOUT, &h.in, &h.out, nullptr, nullptr, nullptr);
    CHECK(std::strcmp(h.out.return_msg, OLMKK_ABOUT) == 0, "about");

    Put(h, 1, 1, {255, 10, 20, 30});
    ZeroLengths(h);
    CHECK(Render(h) == PF_Err_NONE, "copy render");
    CHECK(h.pre_flags == PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS, "extra");
    CHECK(h.last_request.left == 1 && h.last_request.top == 2, "request");
    PF_Pixel got = Get(h, 1, 1);
    CHECK(got.alpha == 255 && got.red == 10 && got.green == 20 && got.blue == 30, "copied");
    CHECK(h.out_bytes[0] == 0x5A, "padding kept");
}

void TestStreak() {
    Host h;
    Init(h, 7, 7);
    ZeroLengths(h);
    h.params[OLMKK_V_LEN].u.sd.value = 4;
    h.params[OLMKK_BLUR_MODE].u.pd.value = 1;
    h.params[OLMKK_GAIN].u.fs_d.value = 10;
    h.params[OLMKK_SRC_OPACITY].u.sd.value = 0;
    h.params[OLMKK_GLOW_OPACITY].u.sd.value = 100;
    h.params[OLMKK_STRENGTH].u.sd.value = 100;
    for (int y = 0; y < 7; ++y)
        for (int x = 0; x < 7; ++x) Put(h, x, y, {0, 0, 0, 0});
    Put(h, 3, 3, {255, 255, 255, 255});
    CHECK(Render(h) == PF_Err_NONE, "streak render");
    PF_Pixel c = Get(h, 3, 3), up = Get(h, 3, 2), side = Get(h, 5, 3), corner = Get(h, 0, 0);
    CHECK(c.alpha > 0, "center lit");
    CHECK(up.alpha > side.alpha, "vertical streak beats horizontal");
    CHECK(corner.alpha == 0, "far corner stays clear");
}

void SetSpark(Host &h, int channel, int blur, int vlen) {
    ZeroLengths(h);
    h.params[OLMKK_CHANNEL].u.pd.value = channel;
    h.params[OLMKK_BLUR_MODE].u.pd.value = blur;
    h.params[OLMKK_V_LEN].u.sd.value = vlen;
    h.params[OLMKK_GAIN].u.fs_d.value = 10;
    h.params[OLMKK_SRC_OPACITY].u.sd.value = 0;
    h.params[OLMKK_GLOW_OPACITY].u.sd.value = 100;
    h.params[OLMKK_STRENGTH].u.sd.value = 100;
    h.params[OLMKK_MERGE].u.pd.value = 1;
    for (int y = 0; y < h.h; ++y)
        for (int x = 0; x < h.w; ++x) Put(h, x, y, {0, 0, 0, 0});
}

void TestChannels() {
    // Luminance / Brightness modes: opaque white sparks.
    for (int ch = 2; ch <= 4; ++ch) {
        Host h;
        Init(h, 7, 7);
        SetSpark(h, ch, 1, 4);
        Put(h, 3, 3, {255, 255, 255, 255});
        CHECK(Render(h) == PF_Err_NONE, "channel render");
        CHECK(Get(h, 3, 3).alpha > 0, "channel sparks center");
        CHECK(Get(h, 3, 2).alpha > Get(h, 5, 3).alpha, "channel streaks vertical");
    }
    // Color (RGB) mode: spark carries the source hue, not the param color.
    Host h;
    Init(h, 7, 7);
    SetSpark(h, 3, 1, 4);
    h.params[OLMKK_V_COLOR].u.cd.value.red = 0;  // param says green/blue only
    Put(h, 3, 3, {255, 255, 0, 0});              // source is red
    CHECK(Render(h) == PF_Err_NONE, "color render");
    PF_Pixel c = Get(h, 3, 3);
    CHECK(c.red > c.green && c.red > c.blue, "color mode takes source hue");
    // Luminance mode ignores a transparent bright pixel.
    Host t;
    Init(t, 7, 7);
    SetSpark(t, 2, 1, 4);
    Put(t, 3, 3, {0, 255, 255, 255});  // alpha 0
    CHECK(Render(t) == PF_Err_NONE, "lum render");
    CHECK(Get(t, 3, 3).alpha == 0, "luminance needs alpha");
}

void TestBlurModes() {
    for (int m = 1; m <= 4; ++m) {
        Host h;
        Init(h, 7, 7);
        SetSpark(h, 1, m, 4);
        Put(h, 3, 3, {255, 255, 255, 255});
        CHECK(Render(h) == PF_Err_NONE, "blur mode render");
        CHECK(Get(h, 3, 3).alpha > 0, "blur mode lights center");
    }
    // Highlight glow only (no arms): radius 3, mode 1.
    Host h;
    Init(h, 7, 7);
    ZeroLengths(h);
    h.params[OLMKK_CHANNEL].u.pd.value = 1;
    h.params[OLMKK_BLUR_MODE].u.pd.value = 1;
    h.params[OLMKK_HL_RADIUS].u.sd.value = 3;
    h.params[OLMKK_GAIN].u.fs_d.value = 10;
    h.params[OLMKK_SRC_OPACITY].u.sd.value = 0;
    h.params[OLMKK_GLOW_OPACITY].u.sd.value = 100;
    h.params[OLMKK_STRENGTH].u.sd.value = 100;
    for (int y = 0; y < 7; ++y)
        for (int x = 0; x < 7; ++x) Put(h, x, y, {0, 0, 0, 0});
    Put(h, 3, 3, {255, 255, 255, 255});
    CHECK(Render(h) == PF_Err_NONE, "glow render");
    CHECK(Get(h, 3, 3).alpha > 0 && Get(h, 4, 3).alpha > 0, "glow spreads");
}

void TestMergeAdd() {
    Host h;
    Init(h, 7, 7);
    SetSpark(h, 1, 1, 4);
    h.params[OLMKK_MERGE].u.pd.value = 2;  // add
    h.params[OLMKK_SRC_OPACITY].u.sd.value = 100;
    for (int y = 0; y < 7; ++y)
        for (int x = 0; x < 7; ++x) Put(h, x, y, {255, 40, 40, 40});
    CHECK(Render(h) == PF_Err_NONE, "merge add");
    PF_Pixel c = Get(h, 3, 3);
    CHECK(c.alpha == 255 && c.red >= 40, "add keeps source and boosts");
    // Invalid merge value leaves the output untouched (binary never writes dst).
    Host t;
    Init(t, 4, 4);
    SetSpark(t, 1, 1, 3);
    t.params[OLMKK_MERGE].u.pd.value = 9;
    Put(t, 1, 1, {255, 255, 255, 255});
    CHECK(Render(t) == PF_Err_NONE, "bad merge render");
    CHECK(t.out_bytes[0] == 0x5A, "bad merge leaves output");
}

void TestDownsample() {
    // Half resolution: sizes halve, so a length-2 arm truncates to 1.
    Host h;
    Init(h, 8, 8);
    SetSpark(h, 1, 1, 2);
    h.params[OLMKK_APPROX].u.bd.value = 1;
    h.in.downsample_x = {1, 1};  // full res but approx on: scale stays 1, no half
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) Put(h, x, y, {0, 0, 0, 0});
    Put(h, 4, 4, {255, 255, 255, 255});
    CHECK(Render(h) == PF_Err_NONE, "approx render");
    CHECK(Get(h, 4, 4).alpha > 0, "approx sparks");
    // Downsampled: scale 0.5 with approx -> half-res path runs.
    Host d;
    Init(d, 8, 8);
    SetSpark(d, 1, 1, 8);
    d.params[OLMKK_APPROX].u.bd.value = 1;
    d.in.downsample_x = {1, 2};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) Put(d, x, y, {0, 0, 0, 0});
    Put(d, 4, 4, {255, 255, 255, 255});
    CHECK(Render(d) == PF_Err_NONE, "half-res render");
    CHECK(Get(d, 4, 4).alpha > 0, "half-res sparks");
}

void TestRamp() {
    Host h;
    Init(h, 2, 2);
    PF_ArbParamsExtra ex{};
    ex.which_function = PF_Arbitrary_FLAT_SIZE_FUNC;
    A_u_long sz = 0;
    ex.u.flat_size_func_params.arbH = h.params[OLMKK_V_RAMP].u.arb_d.dephault;
    ex.u.flat_size_func_params.flat_data_sizePLu = &sz;
    CHECK(EffectMain(PF_Cmd_ARBITRARY_CALLBACK, &h.in, &h.out, nullptr, nullptr, &ex) == 0, "flat size call");
    CHECK(sz == 325, "flat size");
    unsigned char buf[325];
    ex.which_function = PF_Arbitrary_FLATTEN_FUNC;
    ex.u.flatten_func_params.arbH = h.params[OLMKK_V_RAMP].u.arb_d.dephault;
    ex.u.flatten_func_params.buf_sizeLu = 325;
    ex.u.flatten_func_params.flat_dataPV = buf;
    CHECK(EffectMain(PF_Cmd_ARBITRARY_CALLBACK, &h.in, &h.out, nullptr, nullptr, &ex) == 0, "flatten");
    CHECK(buf[0] == 1, "version");
    int count = 0;
    std::memcpy(&count, buf + 1, 4);
    CHECK(count == 3, "default stop count");

    PF_ParamDef *ptrs[OLMKK_NUM_PARAMS] = {};
    for (int i = 0; i < OLMKK_NUM_PARAMS; ++i) ptrs[i] = &h.params[i];
    h.params[OLMKK_V_USE_RAMP].u.bd.value = 1;
    h.disabled.clear();
    CHECK(EffectMain(PF_Cmd_UPDATE_PARAMS_UI, &h.in, &h.out, ptrs, nullptr, nullptr) == 0, "ui");
    bool color_off = false, ramp_on = false;
    for (auto &d : h.disabled) {
        if (d.first == OLMKK_V_COLOR) color_off = d.second;
        if (d.first == OLMKK_V_RAMP) ramp_on = !d.second;
    }
    CHECK(color_off && ramp_on, "use ramp disables color");
}

}  // namespace

int main() {
    TestIdentity();
    TestStreak();
    TestChannels();
    TestBlurModes();
    TestMergeAdd();
    TestDownsample();
    TestRamp();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

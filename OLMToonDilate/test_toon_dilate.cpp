// Fake host. Production EffectMain: GLOBAL_SETUP, PARAMS_SETUP, SMART_PRE_RENDER,
// SMART_RENDER. Hand-checked Chebyshev dilate, tie-break, and downsample ceil.
#include "OLMToonDilate.cpp"

#include <algorithm>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
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
    int depth = 8, w = 0, h = 0;
    PF_UtilCallbacks utils{};
    PF_InData in{};
    PF_OutData out{};
    std::vector<unsigned char> in_bytes, out_bytes;
    PF_EffectWorld input{}, output{};
    PF_ParamDef params[OLMTD_NUM_PARAMS]{};
    int add_count = 0;
    PF_Rect last_request{};
    A_short pre_flags = 0;
    PF_LRect result_rect{};
};

Host *g = nullptr;

PF_Err add_param(PF_ProgPtr, PF_ParamIndex, PF_ParamDefPtr def) {
    if (g->add_count >= OLMTD_NUM_PARAMS - 1) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    g->params[++g->add_count] = *def;
    return PF_Err_NONE;
}
PF_Err checkout_param(PF_ProgPtr, PF_ParamIndex i, A_long, A_long, A_u_long, PF_ParamDef *p) {
    if (i <= 0 || i >= OLMTD_NUM_PARAMS) return PF_Err_BAD_CALLBACK_PARAM;
    *p = g->params[i];
    return PF_Err_NONE;
}
PF_Err checkin_param(PF_ProgPtr, PF_ParamDef *) { return PF_Err_NONE; }
PF_Err copy_world(PF_ProgPtr, PF_EffectWorld *s, PF_EffectWorld *d, PF_Rect *, PF_Rect *) {
    if (s->rowbytes != d->rowbytes || s->height != d->height) return PF_Err_INTERNAL_STRUCT_DAMAGED;
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

size_t PixSize(int d) { return d == 8 ? 4 : d == 16 ? 8 : 16; }

void SetWorld(PF_EffectWorld &w, std::vector<unsigned char> &b, int width, int height, int d) {
    const A_long rb = (A_long)(width * PixSize(d) + 24);
    b.assign((size_t)rb * (size_t)height, 0x5A);
    AEFX_CLR_STRUCT(w);
    w.data = (PF_PixelPtr)b.data();
    w.rowbytes = rb;
    w.width = width;
    w.height = height;
}

void Init(Host &h, int d, int width, int height) {
    g = &h;
    h.depth = d;
    h.w = width;
    h.h = height;
    SetWorld(h.input, h.in_bytes, width, height, d);
    SetWorld(h.output, h.out_bytes, width, height, d);
    h.utils.ansi.sprintf = mock_sprintf;
    h.utils.copy = copy_world;
    h.in.inter.add_param = add_param;
    h.in.inter.checkout_param = checkout_param;
    h.in.inter.checkin_param = checkin_param;
    h.in.utils = &h.utils;
    h.in.effect_ref = (PF_ProgPtr)&h;
    h.in.time_step = 1;
    h.in.time_scale = 30;
    h.in.downsample_x = {1, 1};
    h.in.downsample_y = {2, 1};
    h.add_count = 0;
    EffectMain(PF_Cmd_GLOBAL_SETUP, &h.in, &h.out, nullptr, nullptr, nullptr);
    EffectMain(PF_Cmd_PARAMS_SETUP, &h.in, &h.out, nullptr, nullptr, nullptr);
}

struct Px { float a, r, g, b; };

Px Get(Host &h, PF_EffectWorld &w, int x, int y) {
    char *row = (char *)w.data + (size_t)y * w.rowbytes;
    if (h.depth == 8) {
        PF_Pixel p = ((PF_Pixel *)row)[x];
        return {p.alpha / 255.f, p.red / 255.f, p.green / 255.f, p.blue / 255.f};
    }
    if (h.depth == 16) {
        PF_Pixel16 p = ((PF_Pixel16 *)row)[x];
        return {p.alpha / 32768.f, p.red / 32768.f, p.green / 32768.f, p.blue / 32768.f};
    }
    PF_PixelFloat p = ((PF_PixelFloat *)row)[x];
    return {p.alpha, p.red, p.green, p.blue};
}

void Put(Host &h, int x, int y, Px v) {
    char *row = (char *)h.input.data + (size_t)y * h.input.rowbytes;
    if (h.depth == 8) {
        ((PF_Pixel *)row)[x] = {(A_u_char)(v.a * 255.f + 0.5f), (A_u_char)(v.r * 255.f + 0.5f),
                                (A_u_char)(v.g * 255.f + 0.5f), (A_u_char)(v.b * 255.f + 0.5f)};
    } else if (h.depth == 16) {
        ((PF_Pixel16 *)row)[x] = {(A_u_short)(v.a * 32768.f + 0.5f), (A_u_short)(v.r * 32768.f + 0.5f),
                                  (A_u_short)(v.g * 32768.f + 0.5f), (A_u_short)(v.b * 32768.f + 0.5f)};
    } else {
        ((PF_PixelFloat *)row)[x] = {v.a, v.r, v.g, v.b};
    }
}

void Fill(Host &h, Px v) {
    for (int y = 0; y < h.h; ++y)
        for (int x = 0; x < h.w; ++x) Put(h, x, y, v);
}

PF_Err Render(Host &h) {
    PF_PreRenderInput pin{};
    PF_PreRenderOutput pout{};
    PF_PreRenderCallbacks pcb{};
    PF_PreRenderExtra pex{};
    pin.output_request.rect = {1, 2, h.w - 1, h.h - 1};
    pcb.checkout_layer = pre_checkout;
    pex.input = &pin;
    pex.output = &pout;
    pex.cb = &pcb;
    PF_Err err = EffectMain(PF_Cmd_SMART_PRE_RENDER, &h.in, &h.out, nullptr, nullptr, &pex);
    h.pre_flags = pout.flags;
    h.result_rect = pout.result_rect;
    PF_SmartRenderInput rin{};
    PF_SmartRenderCallbacks rcb{};
    PF_SmartRenderExtra rex{};
    rin.bitdepth = (short)h.depth;
    rcb.checkout_layer_pixels = co_layer;
    rcb.checkout_output = co_output;
    rex.input = &rin;
    rex.cb = &rcb;
    return err ? err : EffectMain(PF_Cmd_SMART_RENDER, &h.in, &h.out, nullptr, nullptr, &rex);
}

bool Same(Px a, Px b) {
    return a.a == b.a && a.r == b.r && a.g == b.g && a.b == b.b;
}

const Px kClear{0, 0, 0, 0};
const Px kRed{1, 1, 0, 0};
const Px kGreen{1, 0, 1, 0};
const Px kBlue{1, 0, 0, 1};
const Px kAlmost{254.f / 255.f, 1, 1, 1};

void TestIdentity() {
    Host h;
    Init(h, 8, 4, 3);
    CHECK(h.out.my_version == 559104, "version");
    CHECK(h.out.out_flags == 0x02000044, "out_flags");
    CHECK(h.out.out_flags2 == 0x08021400, "out_flags2");
    CHECK(h.out.num_params == 2, "num_params");
    CHECK(std::strcmp(h.params[1].PF_DEF_NAME, "Search Radius") == 0, "name");
    CHECK(h.params[1].uu.id == 1, "id");
    CHECK(h.params[1].u.fs_d.value == 2.0, "default");
    CHECK(h.params[1].u.fs_d.valid_min == 0 && h.params[1].u.fs_d.valid_max == 100, "range");
    CHECK(h.params[1].u.fs_d.precision == 1 && h.params[1].flags == 0, "precision/flags");
    EffectMain(PF_Cmd_ABOUT, &h.in, &h.out, nullptr, nullptr, nullptr);
    CHECK(std::strcmp(h.out.return_msg, OLMTD_ABOUT) == 0, "about");
    // Literal text from the Windows binary (ABOUT formats "%s %d.%d.%d\r%s" with 1,1,1).
    CHECK(std::strcmp(h.out.return_msg, "OLM Toon Dilate 1.1.1\rToon Dilate Effect") == 0, "about literal");

    Fill(h, kAlmost);
    Put(h, 1, 1, kRed);
    h.params[1].u.fs_d.value = 0;
    CHECK(Render(h) == PF_Err_NONE, "render 0");
    CHECK(h.pre_flags == PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS, "extra pixels");
    CHECK(h.last_request.left == 1 && h.last_request.top == 2, "request not expanded");
    CHECK(h.result_rect.left == 1 && h.result_rect.top == 2, "result union");
    bool same = true;
    for (int y = 0; y < h.h; ++y)
        for (int x = 0; x < h.w; ++x) same &= Same(Get(h, h.output, x, y), Get(h, h.input, x, y));
    CHECK(same, "radius 0 is copy");
}

void TestSquare(int depth) {
    Host h;
    Init(h, depth, 5, 5);
    Fill(h, kClear);
    Put(h, 2, 2, kRed);
    h.params[1].u.fs_d.value = 1;
    CHECK(Render(h) == PF_Err_NONE, "square render");
    bool ok = true;
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 5; ++x) {
            const int d = std::max(std::abs(x - 2), std::abs(y - 2));
            ok &= Same(Get(h, h.output, x, y), d <= 1 ? kRed : kClear);
        }
    CHECK(ok, "radius 1 square");
    h.params[1].u.fs_d.value = 2;
    std::memcpy(h.output.data, h.in_bytes.data(), h.out_bytes.size());
    // refill input (output copy doesn't matter; render copies input)
    Fill(h, kClear);
    Put(h, 2, 2, kRed);
    CHECK(Render(h) == PF_Err_NONE, "square 2");
    ok = true;
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 5; ++x) {
            const int d = std::max(std::abs(x - 2), std::abs(y - 2));
            ok &= Same(Get(h, h.output, x, y), d <= 2 ? kRed : kClear);
        }
    CHECK(ok, "radius 2 fills the 5x5");
}

void TestTie() {
    Host h;
    Init(h, 8, 3, 3);
    Fill(h, kClear);
    Put(h, 0, 1, kGreen);
    Put(h, 2, 1, kBlue);
    h.params[1].u.fs_d.value = 1;
    CHECK(Render(h) == PF_Err_NONE, "tie render");
    CHECK(Same(Get(h, h.output, 1, 1), kGreen), "center keeps left seed");
    CHECK(Same(Get(h, h.output, 1, 0), kBlue), "top-center from down-right");
    CHECK(Same(Get(h, h.output, 1, 2), kGreen), "bottom-center from up-left");
    CHECK(Same(Get(h, h.output, 0, 1), kGreen), "green seed stays");
    CHECK(Same(Get(h, h.output, 2, 1), kBlue), "blue seed stays");
}

void TestAlmost() {
    Host h;
    Init(h, 8, 3, 3);
    Fill(h, kAlmost);
    h.params[1].u.fs_d.value = 5;
    CHECK(Render(h) == PF_Err_NONE, "almost");
    bool same = true;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x) same &= Same(Get(h, h.output, x, y), kAlmost);
    CHECK(same, "alpha 254 is not a seed");
}

void TestBackward() {
    Host h;
    Init(h, 8, 5, 1);
    Fill(h, kClear);
    Put(h, 4, 0, kRed);
    h.params[1].u.fs_d.value = 2;
    CHECK(Render(h) == PF_Err_NONE, "backward");
    CHECK(Same(Get(h, h.output, 4, 0), kRed), "seed");
    CHECK(Same(Get(h, h.output, 3, 0), kRed), "d1");
    CHECK(Same(Get(h, h.output, 2, 0), kRed), "d2");
    CHECK(Same(Get(h, h.output, 1, 0), kClear), "d3 stays");
    CHECK(Same(Get(h, h.output, 0, 0), kClear), "d4 stays");
}

void TestDownsample() {
    Host h;
    Init(h, 8, 5, 1);
    Fill(h, kClear);
    Put(h, 0, 0, kRed);
    h.params[1].u.fs_d.value = 2;
    h.in.downsample_x = {1, 2};
    CHECK(Render(h) == PF_Err_NONE, "ds");
    CHECK(Same(Get(h, h.output, 1, 0), kRed), "ceil(1.0) still reaches d1");
    CHECK(Same(Get(h, h.output, 2, 0), kClear), "effective radius 1 stops at d2");
}

}  // namespace

int main() {
    TestIdentity();
    TestSquare(8);
    TestSquare(16);
    TestSquare(32);
    TestTie();
    TestAlmost();
    TestBackward();
    TestDownsample();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

// Fake AE host; production EffectMain runs PARAMS_SETUP -> UPDATE_PARAMS_UI ->
// SMART_PRE_RENDER -> SMART_RENDER. Covers: 13-param identities/defaults,
// UI disable rules, the OpenCV-equivalent primitives (exact EDT vs brute
// force incl. the "border is not a zero" rule, nearest/linear resize, min-max
// normalise, box/Gaussian kernels), and end-to-end Inside/Outside/Both with
// Constant/Linear/Sphere/Power, Invert, Layer/RGB colour, background, blur,
// degenerate masks and downsampling at 8/16/32 bpc.
#include "OLMDistanceGradation.cpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {

int g_checks = 0, g_failures = 0;
#define CHECK(cond, msg)                                         \
    do {                                                         \
        ++g_checks;                                              \
        if (!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__);  \
            ++g_failures;                                        \
        }                                                        \
    } while (0)

struct Host {
    int depth = 8, w = 0, h = 0;
    SPBasicSuite basic{};
    PF_UtilCallbacks utils{};
    PF_InData in{};
    PF_OutData out{};
    std::vector<unsigned char> in_bytes, out_bytes;
    PF_EffectWorld input{}, output{};
    PF_ParamDef params[OLMDG_NUM_PARAMS]{};
    PF_ParamDef *ptrs[OLMDG_NUM_PARAMS]{};
    int add_count = 0, checkouts = 0, checkins = 0, layer_out = 0, layer_in = 0;
    std::vector<std::pair<PF_ParamIndex, bool>> disabled;
    PF_ColorParamSuite1 color{};
    PF_ParamUtilsSuite3 utils3{};
};
Host *g = nullptr;

PF_Err add_param(PF_ProgPtr, PF_ParamIndex, PF_ParamDefPtr def) {
    if (g->add_count >= OLMDG_NUM_PARAMS - 1) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    g->params[++g->add_count] = *def;
    return PF_Err_NONE;
}
PF_Err checkout_param(PF_ProgPtr, PF_ParamIndex i, A_long, A_long, A_u_long, PF_ParamDef *p) {
    if (i <= 0 || i >= OLMDG_NUM_PARAMS) return PF_Err_BAD_CALLBACK_PARAM;
    ++g->checkouts;
    *p = g->params[i];
    return PF_Err_NONE;
}
PF_Err checkin_param(PF_ProgPtr, PF_ParamDef *) { ++g->checkins; return PF_Err_NONE; }
int SPAPI mock_sprintf(A_char *buf, const A_char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    const int n = std::vsnprintf(buf, 256, fmt, a);
    va_end(a);
    return n;
}
PF_Err get_color(PF_ProgPtr, const PF_ParamDef *d, PF_PixelFloat *o) {
    *o = {d->u.cd.value.alpha / 255.0f, d->u.cd.value.red / 255.0f, d->u.cd.value.green / 255.0f,
          d->u.cd.value.blue / 255.0f};
    return PF_Err_NONE;
}
PF_Err SPAPI update_ui(PF_ProgPtr, PF_ParamIndex i, const PF_ParamDef *d) {
    if (d->param_type != g->params[i].param_type) return PF_Err_BAD_CALLBACK_PARAM;
    g->disabled.push_back({i, (d->ui_flags & PF_PUI_DISABLED) != 0});
    return PF_Err_NONE;
}
SPErr SPAPI acquire(const char *n, int32 v, const void **s) {
    if (!std::strcmp(n, kPFColorParamSuite) && v == kPFColorParamSuiteVersion1) *s = &g->color;
    else if (!std::strcmp(n, kPFParamUtilsSuite) && v == kPFParamUtilsSuiteVersion3) *s = &g->utils3;
    else return (SPErr)-1;
    return kSPNoError;
}
SPErr SPAPI release(const char *, int32) { return kSPNoError; }
PF_Err co_layer(PF_ProgPtr, A_long, PF_EffectWorld **w) { ++g->layer_out; *w = &g->input; return PF_Err_NONE; }
PF_Err ci_layer(PF_ProgPtr, A_long) { ++g->layer_in; return PF_Err_NONE; }
PF_Err co_output(PF_ProgPtr, PF_EffectWorld **w) { *w = &g->output; return PF_Err_NONE; }
PF_Err pre_checkout(PF_ProgPtr, PF_ParamIndex, A_long, const PF_RenderRequest *r, A_long, A_long,
                    A_u_long, PF_CheckoutResult *res) {
    res->result_rect = res->max_result_rect = r->rect;
    return PF_Err_NONE;
}

size_t PixSize(int d) { return d == 8 ? 4 : d == 16 ? 8 : 16; }
float MaxV(int d) { return d == 8 ? 255.0f : d == 16 ? 32768.0f : 1.0f; }

void SetWorld(PF_EffectWorld &w, std::vector<unsigned char> &b, int width, int height, int d) {
    const A_long rb = (A_long)(width * PixSize(d) + 32);
    b.assign((size_t)rb * height, 0);
    AEFX_CLR_STRUCT(w);
    w.data = (PF_PixelPtr)b.data();
    w.rowbytes = rb;
    w.width = width;
    w.height = height;
    w.extent_hint = {0, 0, width, height};
}

void Init(Host &h, int d, int width, int height) {
    g = &h;
    h.depth = d;
    h.w = width;
    h.h = height;
    SetWorld(h.input, h.in_bytes, width, height, d);
    SetWorld(h.output, h.out_bytes, width, height, d);
    h.color.PF_GetFloatingPointColorFromColorDef = get_color;
    h.utils3.PF_UpdateParamUI = update_ui;
    h.basic.AcquireSuite = acquire;
    h.basic.ReleaseSuite = release;
    h.utils.ansi.sprintf = mock_sprintf;
    h.in.inter.add_param = add_param;
    h.in.inter.checkout_param = checkout_param;
    h.in.inter.checkin_param = checkin_param;
    h.in.utils = &h.utils;
    h.in.effect_ref = (PF_ProgPtr)&h;
    h.in.pica_basicP = &h.basic;
    h.in.time_step = 1;
    h.in.time_scale = 30;
    h.in.downsample_x = {1, 1};
    h.in.downsample_y = {1, 1};
    EffectMain(PF_Cmd_GLOBAL_SETUP, &h.in, &h.out, nullptr, nullptr, nullptr);
    EffectMain(PF_Cmd_PARAMS_SETUP, &h.in, &h.out, nullptr, nullptr, nullptr);
    for (int i = 0; i < OLMDG_NUM_PARAMS; ++i) h.ptrs[i] = &h.params[i];
    h.params[OLMDG_IN_OUT].u.pd.value = OLMDG_INSIDE;
}

PF_Err Render(Host &h) {
    PF_PreRenderInput pin{};
    PF_PreRenderOutput pout{};
    PF_PreRenderCallbacks pcb{};
    PF_PreRenderExtra pex{};
    pin.output_request.rect = {0, 0, h.w, h.h};
    pcb.checkout_layer = pre_checkout;
    pex.input = &pin;
    pex.output = &pout;
    pex.cb = &pcb;
    PF_Err err = EffectMain(PF_Cmd_SMART_PRE_RENDER, &h.in, &h.out, nullptr, nullptr, &pex);
    PF_SmartRenderInput rin{};
    PF_SmartRenderCallbacks rcb{};
    PF_SmartRenderExtra rex{};
    rin.bitdepth = (short)h.depth;
    rcb.checkout_layer_pixels = co_layer;
    rcb.checkin_layer_pixels = ci_layer;
    rcb.checkout_output = co_output;
    rex.input = &rin;
    rex.cb = &rcb;
    return err ? err : EffectMain(PF_Cmd_SMART_RENDER, &h.in, &h.out, nullptr, nullptr, &rex);
}

struct Px { float a, r, g, b; };  // normalized 0..1
Px Get(Host &h, PF_EffectWorld &w, int x, int y) {
    char *row = (char *)w.data + (size_t)y * w.rowbytes;
    const float m = MaxV(h.depth);
    if (h.depth == 8) { PF_Pixel p = ((PF_Pixel *)row)[x]; return {p.alpha / m, p.red / m, p.green / m, p.blue / m}; }
    if (h.depth == 16) { PF_Pixel16 p = ((PF_Pixel16 *)row)[x]; return {p.alpha / m, p.red / m, p.green / m, p.blue / m}; }
    PF_PixelFloat p = ((PF_PixelFloat *)row)[x];
    return {p.alpha, p.red, p.green, p.blue};
}
void Put(Host &h, int x, int y, Px v) {
    char *row = (char *)h.input.data + (size_t)y * h.input.rowbytes;
    const float m = MaxV(h.depth);
    if (h.depth == 8) ((PF_Pixel *)row)[x] = {(A_u_char)(v.a * m), (A_u_char)(v.r * m), (A_u_char)(v.g * m), (A_u_char)(v.b * m)};
    else if (h.depth == 16) ((PF_Pixel16 *)row)[x] = {(A_u_short)(v.a * m), (A_u_short)(v.r * m), (A_u_short)(v.g * m), (A_u_short)(v.b * m)};
    else ((PF_PixelFloat *)row)[x] = {v.a, v.r, v.g, v.b};
}

// Opaque grey square [x0,x1) x [y0,y1) on transparent.
void Square(Host &h, int x0, int y0, int x1, int y1) {
    for (int y = 0; y < h.h; ++y)
        for (int x = 0; x < h.w; ++x) {
            const bool on = x >= x0 && x < x1 && y >= y0 && y < y1;
            Put(h, x, y, on ? Px{1, 0.5f, 0.25f, 0.75f} : Px{0, 0, 0, 0});
        }
}

void TestPrimitives() {
    // EDT vs brute force (nearest zero; none -> 1e15 squared sentinel).
    std::mt19937 rng(7);
    for (int trial = 0; trial < 20; ++trial) {
        const int n = 3 + rng() % 17, m = 3 + rng() % 13;
        Mask mk((size_t)n * m);
        for (auto &v : mk) v = (rng() % 5) ? 255 : 0;
        if (trial == 0) std::fill(mk.begin(), mk.end(), 255);  // no zeros at all
        const Plane d = DistanceTransform(mk, n, m);
        bool ok = true;
        for (int y = 0; y < m; ++y)
            for (int x = 0; x < n; ++x) {
                double best = 1e30;
                for (int yy = 0; yy < m; ++yy)
                    for (int xx = 0; xx < n; ++xx)
                        if (!mk[(size_t)yy * n + xx])
                            best = std::min(best, std::sqrt((double)(x - xx) * (x - xx) + (double)(y - yy) * (y - yy)));
                const float got = d[(size_t)y * n + x];
                if (best > 1e29) ok &= got > 1e6f;  // border is not a zero
                else ok &= std::fabs(got - best) <= 1e-4 * std::max(1.0, best);
            }
        CHECK(ok, "EDT matches brute force");
    }
    // Nearest upscale 3 -> 6 duplicates; linear identity/averaging.
    Mask s = {0, 255, 0};
    Mask up = ResizeNearest(s, 3, 1, 6, 1);
    CHECK(up == Mask({0, 0, 255, 255, 0, 0}), "nearest x2");
    Plane l = ResizeLinear(Plane({0, 2, 4, 6}), 4, 1, 2, 1);
    CHECK(l[0] == 1 && l[1] == 5, "linear /2 averages pairs");
    Plane nz = {2, 4, 6};
    NormalizeMinMax(nz, 255);
    CHECK(nz[0] == 0 && nz[1] == 127.5f && nz[2] == 255, "minmax normalize");
    Plane flat = {3, 3, 3};
    NormalizeMinMax(flat, 255);
    CHECK(flat[0] == 0 && flat[2] == 0, "flat normalizes to 0");
    std::vector<float> k3 = GaussianKernel(3), k9 = GaussianKernel(9);
    CHECK(k3[0] == 0.25f && k3[1] == 0.5f, "small gaussian table");
    double sum = 0;
    for (float v : k9) sum += v;
    CHECK(std::fabs(sum - 1) < 1e-6 && k9[4] > k9[3] && k9[3] > k9[0], "gaussian 9 normalized");
    Plane c(30, 7.0f);
    Plane bb = BoxBlur(c, 6, 5, 5, 3), gb = GaussianBlur(c, 6, 5, 9, 5);
    bool same = true;
    for (size_t i = 0; i < c.size(); ++i) same &= std::fabs(bb[i] - 7) < 1e-5f && std::fabs(gb[i] - 7) < 1e-5f;
    CHECK(same, "blurs preserve constants (replicate border)");
}

void TestSetupUi() {
    Host h;
    Init(h, 8, 4, 4);
    CHECK(h.out.num_params == 13 && h.add_count == 12, "13 params");
    CHECK(h.out.my_version == 266752 && h.out.out_flags == 0x06000040 && h.out.out_flags2 == 0x08001400,
          "version/flags");
    const PF_ParamDef *p = h.params;
    bool ids = true;
    for (int i = 1; i < 13; ++i) ids &= p[i].uu.id == i;
    CHECK(ids, "ids == positions");
    CHECK(p[2].param_type == PF_Param_POPUP && p[2].u.pd.num_choices == 3 && p[2].u.pd.dephault == 0,
          "In/Out default 0 (binary)");
    CHECK(p[3].u.sd.valid_max == 1000 && p[3].u.sd.slider_max == 512 && p[3].u.sd.dephault == 128, "Inside Threshold");
    CHECK(p[7].u.cd.value.red == 255 && p[7].u.cd.value.green == 0 && p[7].u.cd.value.alpha == 255, "Gradation Color red");
    CHECK(!std::strcmp(p[8].PF_DEF_NAME, "BG Color "), "BG Color name (trailing space)");
    CHECK(p[9].u.pd.dephault == 2 && p[9].u.pd.num_choices == 4, "Interpolation default Linear");
    CHECK(p[10].param_type == PF_Param_FLOAT_SLIDER && p[10].u.fs_d.dephault == 1 &&
              std::fabs(p[10].u.fs_d.valid_min - 0.01f) < 1e-7f && p[10].flags == PF_ParamFlag_COLLAPSE_TWIRLY,
          "Power");
    CHECK(p[12].u.sd.valid_max == 4096 && p[12].u.sd.slider_max == 500 && p[12].flags == PF_ParamFlag_COLLAPSE_TWIRLY,
          "Blur Size");
    EffectMain(PF_Cmd_ABOUT, &h.in, &h.out, nullptr, nullptr, nullptr);
    CHECK(!std::strcmp(h.out.return_msg, "DistanceGradation v0.82\rGenerate color gradation using distance transform."),
          "about");

    auto ui = [&](A_long inout, A_long interp, A_long render, A_long bg, A_long blur) {
        h.params[OLMDG_IN_OUT].u.pd.value = inout;
        h.params[OLMDG_INTERP].u.pd.value = interp;
        h.params[OLMDG_RENDER_MODE].u.pd.value = render;
        h.params[OLMDG_USE_BG].u.bd.value = bg;
        h.params[OLMDG_BLUR_MODE].u.pd.value = blur;
        h.disabled.clear();
        EffectMain(PF_Cmd_UPDATE_PARAMS_UI, &h.in, &h.out, h.ptrs, nullptr, nullptr);
        std::vector<int> st(13, -1);
        for (auto &d : h.disabled) st[d.first] = d.second;
        return st;
    };
    auto s = ui(OLMDG_INSIDE, OLMDG_LINEAR, OLMDG_RENDER_RGB, 0, OLMDG_NO_BLUR);
    CHECK(s[10] == 1 && s[4] == 1 && s[3] == 0 && s[7] == 0 && s[8] == 1 && s[12] == 1, "UI inside/linear/rgb/noBG/noBlur");
    s = ui(OLMDG_OUTSIDE, OLMDG_POWER_MODE, OLMDG_RENDER_LAYER, 1, OLMDG_BLUR);
    CHECK(s[10] == 0 && s[4] == 0 && s[3] == 1 && s[7] == 1 && s[8] == 0 && s[12] == 0, "UI outside/power/layer/BG/blur");
    s = ui(OLMDG_BOTH, OLMDG_CONSTANT, OLMDG_RENDER_RGB, 0, OLMDG_BLUR_NO_SCALE);
    CHECK(s[3] == 0 && s[4] == 0 && s[12] == 0, "UI both enables both thresholds");
}

void TestRender(int depth) {
    const float tol = depth == 8 ? 2.5f / 255 : depth == 16 ? 2e-4f : 1e-5f;
    // Inside, Linear, threshold larger than the shape: alpha falls from the
    // edge toward the centre (t = 1 - normalized distance).
    {
        Host h;
        Init(h, depth, 21, 21);
        Square(h, 3, 3, 18, 18);
        h.params[OLMDG_INSIDE_THR].u.sd.value = 100;
        CHECK(Render(h) == PF_Err_NONE, "render");
        Px edge = Get(h, h.output, 3, 10), mid = Get(h, h.output, 7, 10), centre = Get(h, h.output, 10, 10);
        Px outside = Get(h, h.output, 1, 10);
        CHECK(edge.a > mid.a && mid.a > centre.a, "inside gradient falls toward centre");
        CHECK(centre.a <= tol, "deepest point fully faded");
        CHECK(std::fabs(edge.r - 1) <= tol && edge.g <= tol && edge.b <= tol, "RGB = gradation colour");
        CHECK(outside.a == 0, "transparent stays transparent (Inside)");
        // Edge pixel: distance 1 of max 8 -> t = 1 - 1/8.
        CHECK(std::fabs(edge.a - (1 - 1.0f / 8)) <= tol, "edge value = 1 - d/dmax");
        CHECK(h.layer_out == h.layer_in && h.checkouts == h.checkins, "balanced");

        h.params[OLMDG_INVERT].u.bd.value = 1;
        Render(h);
        CHECK(Get(h, h.output, 10, 10).a >= 1 - tol && Get(h, h.output, 3, 10).a < 0.2f, "Invert flips");

        // Constant, thr 3: pixels deeper than 3 -> t = 0, else t = 1.
        h.params[OLMDG_INVERT].u.bd.value = 0;
        h.params[OLMDG_INTERP].u.pd.value = OLMDG_CONSTANT;
        h.params[OLMDG_INSIDE_THR].u.sd.value = 3;
        Render(h);
        CHECK(Get(h, h.output, 5, 10).a >= 1 - tol && Get(h, h.output, 6, 10).a <= tol, "Constant hard band of 3");

        // Sphere/Power reshape t; Layer mode takes input colour; BG blends.
        h.params[OLMDG_INTERP].u.pd.value = OLMDG_POWER_MODE;
        h.params[OLMDG_INSIDE_THR].u.sd.value = 100;
        h.params[OLMDG_POWER].u.fs_d.value = 2;
        h.params[OLMDG_RENDER_MODE].u.pd.value = OLMDG_RENDER_LAYER;
        Render(h);
        Px pm = Get(h, h.output, 6, 10);  // d = 4 of 8 -> t = 0.5
        CHECK(std::fabs(pm.a - (1 - 4.0f / 8) * (1 - 4.0f / 8)) <= tol, "Power t^2");
        CHECK(std::fabs(pm.r - 0.5f) <= tol && std::fabs(pm.g - 0.25f) <= tol, "Layer colour");
        h.params[OLMDG_INTERP].u.pd.value = OLMDG_SPHERE;
        Render(h);
        const float t = 0.5f;
        CHECK(std::fabs(Get(h, h.output, 6, 10).a - std::sqrt(1 - (1 - t) * (1 - t))) <= tol, "Sphere");
        h.params[OLMDG_INTERP].u.pd.value = OLMDG_LINEAR;
        h.params[OLMDG_RENDER_MODE].u.pd.value = OLMDG_RENDER_RGB;
        h.params[OLMDG_USE_BG].u.bd.value = 1;
        h.params[OLMDG_BG_COLOR].u.cd.value = {255, 0, 0, 255};  // blue
        Render(h);
        Px bgp = Get(h, h.output, 6, 10);
        CHECK(std::fabs(bgp.a - 1) <= tol && std::fabs(bgp.r - 0.5f) <= tol && std::fabs(bgp.b - 0.5f) <= tol,
              "BG blend: alpha = input, RGB = mix");
    }
    // Outside: gradient grows away from the shape, threshold caps it.
    {
        Host h;
        Init(h, depth, 21, 9);
        Square(h, 8, 0, 13, 9);
        h.params[OLMDG_IN_OUT].u.pd.value = OLMDG_OUTSIDE;
        h.params[OLMDG_OUTSIDE_THR].u.sd.value = 4;
        Render(h);
        CHECK(Get(h, h.output, 10, 4).a == 0, "inside of shape transparent (Outside)");
        CHECK(std::fabs(Get(h, h.output, 7, 4).a - 0.75f) <= tol && Get(h, h.output, 2, 4).a <= tol,
              "outside gradient capped at threshold");
        // Both = inside + outside gradations.
        h.params[OLMDG_IN_OUT].u.pd.value = OLMDG_BOTH;
        h.params[OLMDG_INSIDE_THR].u.sd.value = 100;
        Render(h);
        CHECK(Get(h, h.output, 9, 4).a > 0 && Get(h, h.output, 7, 4).a > 0 && Get(h, h.output, 10, 4).a == 0,
              "Both covers inside and outside; deepest point fades");
    }
    // Degenerate: fully opaque -> zero, or BG with Use Background Color.
    {
        Host h;
        Init(h, depth, 6, 5);
        Square(h, 0, 0, 6, 5);
        Render(h);
        CHECK(Get(h, h.output, 2, 2).a == 0 && Get(h, h.output, 2, 2).r == 0, "all-opaque -> transparent");
        h.params[OLMDG_USE_BG].u.bd.value = 1;
        h.params[OLMDG_BG_COLOR].u.cd.value = {255, 0, 255, 0};  // green
        Render(h);
        Px p = Get(h, h.output, 2, 2);
        CHECK(p.a == 1 && std::fabs(p.g - 1) <= tol && p.r == 0, "all-opaque + BG -> BG colour");
    }
    // Downsampling: distances are measured at full resolution.
    {
        Host full, half;
        Init(full, depth, 40, 8);
        Square(full, 10, 0, 30, 8);
        full.params[OLMDG_IN_OUT].u.pd.value = OLMDG_OUTSIDE;
        full.params[OLMDG_INTERP].u.pd.value = OLMDG_CONSTANT;
        full.params[OLMDG_OUTSIDE_THR].u.sd.value = 6;
        Render(full);
        Init(half, depth, 20, 4);
        Square(half, 5, 0, 15, 4);
        half.in.downsample_x = {1, 2};
        half.in.downsample_y = {1, 2};
        half.params[OLMDG_IN_OUT].u.pd.value = OLMDG_OUTSIDE;
        half.params[OLMDG_INTERP].u.pd.value = OLMDG_CONSTANT;
        half.params[OLMDG_OUTSIDE_THR].u.sd.value = 6;
        Render(half);
        int band_full = 0, band_half = 0;
        for (int x = 0; x < 10; ++x) band_full += Get(full, full.output, x, 4).a > 0.5f;
        for (int x = 0; x < 5; ++x) band_half += Get(half, half.output, x, 2).a > 0.5f;
        CHECK(band_full == 6 && band_half == 3, "band scales with downsample");
    }
    // Blur: Gaussian and box smooth the gradation channel (alpha) only.
    {
        Host h;
        Init(h, depth, 21, 21);
        Square(h, 3, 3, 18, 18);
        h.params[OLMDG_INTERP].u.pd.value = OLMDG_CONSTANT;
        h.params[OLMDG_INSIDE_THR].u.sd.value = 3;
        h.params[OLMDG_BLUR_MODE].u.pd.value = OLMDG_BLUR;
        h.params[OLMDG_BLUR_SIZE].u.sd.value = 2;
        Render(h);
        const float a5 = Get(h, h.output, 5, 10).a, a6 = Get(h, h.output, 6, 10).a;
        CHECK(a5 < 1 - tol && a6 > tol && a5 > a6, "Gaussian softens the hard band");
        h.params[OLMDG_BLUR_MODE].u.pd.value = OLMDG_BLUR_NO_SCALE;
        Render(h);
        CHECK(std::fabs(Get(h, h.output, 5, 10).a - 0.6f) <= tol, "box (normalized) 5-tap average");
    }
}

}  // namespace

int main() {
    TestPrimitives();
    TestSetupUi();
    for (int d : {8, 16, 32}) TestRender(d);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

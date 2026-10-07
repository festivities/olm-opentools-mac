// Fake AE host; production EffectMain runs GLOBAL_SETUP -> PARAMS_SETUP ->
// UPDATE_PARAMS_UI -> SMART_PRE_RENDER -> SMART_RENDER. Covers: param
// identities/defaults (Legacy old-project value), Smoothness hiding, pre-render
// full-layer request + RETURNS_EXTRA_PIXELS, amount-0 pass-through, exact match
// against an independent brute-force reference for both bias directions and
// several repeat/downsample settings at 8/16/32 bpc, division preservation
// (no bleed across alpha 0), alpha untouched, Legacy flat-area/column-0 quirks.
#include "OLMBlur.cpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
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
    PF_ParamDef params[OLMBLUR_NUM_PARAMS]{};
    int add_count = 0, checkouts = 0, checkins = 0, layer_out = 0, layer_in = 0;
    std::vector<std::pair<PF_ParamIndex, A_Boolean>> hidden;
    PF_RenderRequest last_request{};
    AEGP_UtilitySuite3 util{};
    AEGP_PFInterfaceSuite1 pfi{};
    AEGP_EffectSuite5 effect{};
    AEGP_StreamSuite7 stream{};
    AEGP_DynamicStreamSuite3 dyn{};
};
Host *g = nullptr;

PF_Err add_param(PF_ProgPtr, PF_ParamIndex, PF_ParamDefPtr def) {
    if (g->add_count >= OLMBLUR_NUM_PARAMS - 1) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    g->params[++g->add_count] = *def;
    return PF_Err_NONE;
}
PF_Err checkout_param(PF_ProgPtr, PF_ParamIndex i, A_long, A_long, A_u_long, PF_ParamDef *p) {
    if (i <= 0 || i >= OLMBLUR_NUM_PARAMS) return PF_Err_BAD_CALLBACK_PARAM;
    ++g->checkouts;
    *p = g->params[i];
    return PF_Err_NONE;
}
PF_Err checkin_param(PF_ProgPtr, PF_ParamDef *) { ++g->checkins; return PF_Err_NONE; }
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
PF_Err reg(AEGP_GlobalRefcon, const A_char *, AEGP_PluginID *id) { *id = 9; return A_Err_NONE; }
PF_Err get_effect(AEGP_PluginID, PF_ProgPtr, AEGP_EffectRefH *e) { *e = (AEGP_EffectRefH)2; return A_Err_NONE; }
PF_Err dispose_effect(AEGP_EffectRefH) { return A_Err_NONE; }
PF_Err get_stream(AEGP_PluginID, AEGP_EffectRefH, PF_ParamIndex i, AEGP_StreamRefH *s) {
    *s = (AEGP_StreamRefH)(ptrdiff_t)(i + 1);
    return A_Err_NONE;
}
PF_Err dispose_stream(AEGP_StreamRefH) { return A_Err_NONE; }
PF_Err set_flag(AEGP_StreamRefH s, AEGP_DynStreamFlags f, A_Boolean undoable, A_Boolean set) {
    if (f != AEGP_DynStreamFlag_HIDDEN || undoable) return (A_Err)1;
    g->hidden.push_back({(PF_ParamIndex)((ptrdiff_t)s - 1), set});
    return A_Err_NONE;
}
SPErr SPAPI acquire(const char *n, int32 v, const void **s) {
    struct { const char *n; int32 v; const void *s; } t[] = {
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
PF_Err co_layer(PF_ProgPtr, A_long, PF_EffectWorld **w) { ++g->layer_out; *w = &g->input; return PF_Err_NONE; }
PF_Err ci_layer(PF_ProgPtr, A_long) { ++g->layer_in; return PF_Err_NONE; }
PF_Err co_output(PF_ProgPtr, PF_EffectWorld **w) { *w = &g->output; return PF_Err_NONE; }
PF_Err pre_checkout(PF_ProgPtr, PF_ParamIndex, A_long, const PF_RenderRequest *r, A_long, A_long,
                    A_u_long, PF_CheckoutResult *res) {
    g->last_request = *r;
    res->result_rect = res->max_result_rect = r->rect;
    return PF_Err_NONE;
}

size_t PixSize(int depth) { return depth == 8 ? 4 : depth == 16 ? 8 : 16; }

void SetWorld(PF_EffectWorld &w, std::vector<unsigned char> &b, int width, int height, int depth) {
    const A_long rb = (A_long)(width * PixSize(depth) + 24);  // padded rows
    b.assign((size_t)rb * height, 0);
    AEFX_CLR_STRUCT(w);
    w.data = (PF_PixelPtr)b.data();
    w.rowbytes = rb;
    w.width = width;
    w.height = height;
    w.extent_hint = {0, 0, width, height};
}

void Init(Host &h, int depth, int width, int height) {
    g = &h;
    h.depth = depth;
    h.w = width;
    h.h = height;
    SetWorld(h.input, h.in_bytes, width, height, depth);
    SetWorld(h.output, h.out_bytes, width, height, depth);
    h.util.AEGP_RegisterWithAEGP = reg;
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
    h.in.width = width;
    h.in.height = height;
    h.in.downsample_x = {1, 1};
    h.in.downsample_y = {1, 1};
    EffectMain(PF_Cmd_GLOBAL_SETUP, &h.in, &h.out, nullptr, nullptr, nullptr);
    EffectMain(PF_Cmd_PARAMS_SETUP, &h.in, &h.out, nullptr, nullptr, nullptr);
}

PF_Err Render(Host &h) {
    PF_PreRenderInput pin{};
    PF_PreRenderOutput pout{};
    PF_PreRenderCallbacks pcb{};
    PF_PreRenderExtra pex{};
    pin.output_request.rect = {1, 1, 2, 2};
    pcb.checkout_layer = pre_checkout;
    pex.input = &pin;
    pex.output = &pout;
    pex.cb = &pcb;
    PF_Err err = EffectMain(PF_Cmd_SMART_PRE_RENDER, &h.in, &h.out, nullptr, nullptr, &pex);
    const PF_LRect &q = h.last_request.rect;
    CHECK(!err && q.left <= 0 && q.top <= 0 && q.right >= h.w && q.bottom >= h.h &&
              q.left == std::min(0, 1) && q.right == std::max(h.w, 2),
          "pre-render request covers the full layer");
    CHECK(pout.flags == PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS, "returns extra pixels");
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

// Generic channel access in native units.
struct Px { float a, r, g, b; };
Px Get(Host &h, PF_EffectWorld &w, int x, int y) {
    char *row = (char *)w.data + (size_t)y * w.rowbytes;
    if (h.depth == 8) { PF_Pixel p = ((PF_Pixel *)row)[x]; return {(float)p.alpha, (float)p.red, (float)p.green, (float)p.blue}; }
    if (h.depth == 16) { PF_Pixel16 p = ((PF_Pixel16 *)row)[x]; return {(float)p.alpha, (float)p.red, (float)p.green, (float)p.blue}; }
    PF_PixelFloat p = ((PF_PixelFloat *)row)[x];
    return {p.alpha, p.red, p.green, p.blue};
}
void Put(Host &h, int x, int y, Px v) {
    char *row = (char *)h.input.data + (size_t)y * h.input.rowbytes;
    if (h.depth == 8) ((PF_Pixel *)row)[x] = {(A_u_char)v.a, (A_u_char)v.r, (A_u_char)v.g, (A_u_char)v.b};
    else if (h.depth == 16) ((PF_Pixel16 *)row)[x] = {(A_u_short)v.a, (A_u_short)v.r, (A_u_short)v.g, (A_u_short)v.b};
    else ((PF_PixelFloat *)row)[x] = {v.a, v.r, v.g, v.b};
}

// Independent brute-force reference of the current (non-Legacy) algorithm.
std::vector<float> Reference(Host &h, float amount, int repeat, int bias, float scale) {
    const int W = h.w, H = h.h;
    std::vector<float> img(3 * W * H), tmp(3 * W * H);
    std::vector<int> m(W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            Px p = Get(h, h.input, x, y);
            img[3 * (y * W + x)] = p.r; img[3 * (y * W + x) + 1] = p.g; img[3 * (y * W + x) + 2] = p.b;
            m[y * W + x] = p.a != 0;
        }
    const float A = amount * scale;
    const float k = repeat >= 2 ? powf(3.0f / A, 1.0f / (float)(repeat - 1)) : 1.0f;
    auto pass = [&](std::vector<float> &src, std::vector<float> &dst, std::vector<float> &lut, int R, bool vertical) {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const int i = y * W + x;
                if (!m[i]) { for (int c = 0; c < 3; ++c) dst[3 * i + c] = src[3 * i + c]; continue; }
                float ws = 0, s[3] = {0, 0, 0};
                for (int dir = -1; dir <= 1; dir += 2)
                    for (int d = dir < 0 ? 0 : 1; d <= R; ++d) {
                        const int xx = vertical ? x : x + dir * d, yy = vertical ? y + dir * d : y;
                        if (xx < 0 || yy < 0 || xx >= W || yy >= H || !m[yy * W + xx]) break;
                        ws = ws + lut[d];
                        for (int c = 0; c < 3; ++c) s[c] = s[c] + lut[d] * src[3 * (yy * W + xx) + c];
                    }
                const float inv = ws == 0 ? 0 : 1.0f / ws;
                for (int c = 0; c < 3; ++c) dst[3 * i + c] = inv * s[c];
            }
    };
    for (int it = 0; it < repeat; ++it) {
        const float r = (float)((double)A * pow((double)k, (double)it));
        const int R = (int)r;
        if (!R) break;
        std::vector<float> lut(R + 1);
        const float sg = r / 3.0f;
        for (int j = 0; j <= R; ++j) lut[j] = expf(-(float)(j * j) / ((sg + sg) * sg));
        pass(img, tmp, lut, R, bias == OLMBLUR_BIAS_HORIZONTAL);
        pass(tmp, img, lut, R, bias != OLMBLUR_BIAS_HORIZONTAL);
    }
    return img;
}

float Quant(Host &h, float v) { return h.depth == 32 ? v : floorf(v + 0.5f); }

// Two opaque regions split by a transparent column; mixed colours inside.
void FillScene(Host &h) {
    const float M = h.depth == 8 ? 255.0f : h.depth == 16 ? 32768.0f : 1.0f;
    for (int y = 0; y < h.h; ++y)
        for (int x = 0; x < h.w; ++x) {
            const bool gap = x == h.w / 2;
            const float v = (float)((x * 37 + y * 91) % 17) / 16.0f;
            Px p = {gap ? 0.0f : M, x < h.w / 2 ? M * v : 0.0f, M * (1 - v) * 0.5f,
                    x < h.w / 2 ? 0.0f : M * v};
            if (gap) p.r = p.g = p.b = M * 0.25f;
            Put(h, x, y, p);
        }
}

void TestSetup() {
    Host h;
    Init(h, 8, 4, 4);
    CHECK(h.out.num_params == 6 && h.add_count == 5, "6 params");
    CHECK(h.out.my_version == 591872 && h.out.out_flags == 0x06000040 &&
              h.out.out_flags2 == 0x08001400, "version and flags");
    const PF_ParamDef *p = h.params;
    CHECK(p[1].uu.id == 5 && p[1].param_type == PF_Param_FLOAT_SLIDER &&
              p[1].u.fs_d.valid_min == 1 && p[1].u.fs_d.valid_max == 1000 &&
              p[1].u.fs_d.slider_max == 50 && p[1].u.fs_d.dephault == 5 &&
              p[1].u.fs_d.precision == 2 && p[1].u.fs_d.curve_tolerance == 0, "Blur Amount");
    CHECK(p[2].uu.id == 6 && p[2].param_type == PF_Param_FIX_SLIDER &&
              p[2].u.fd.value == (100 << 16) && p[2].u.fd.valid_min == (1 << 16) &&
              p[2].u.fd.precision == 1 && p[2].u.fd.display_flags == PF_ValueDisplayFlag_PERCENT,
          "Blur Smoothness");
    CHECK(p[3].uu.id == 3 && p[3].u.sd.valid_min == 1 && p[3].u.sd.valid_max == 10 &&
              p[3].u.sd.dephault == 2, "Number of Repeat");
    CHECK(p[4].uu.id == 4 && p[4].param_type == PF_Param_POPUP && p[4].u.pd.num_choices == 2 &&
              !std::strcmp(p[4].u.pd.u.namesptr, "Vertical|Horizontal"), "Bias Direction");
    CHECK(p[5].uu.id == 7 && p[5].param_type == PF_Param_CHECKBOX && p[5].u.bd.value == 1 &&
              p[5].u.bd.dephault == 0 && p[5].flags == PF_ParamFlag_USE_VALUE_FOR_OLD_PROJECTS,
          "Legacy: new=off, old projects=on");
    EffectMain(PF_Cmd_ABOUT, &h.in, &h.out, nullptr, nullptr, nullptr);
    CHECK(!std::strcmp(h.out.return_msg, OLMBLUR_ABOUT), "about");
    EffectMain(PF_Cmd_UPDATE_PARAMS_UI, &h.in, &h.out, nullptr, nullptr, nullptr);
    CHECK(h.hidden.size() == 1 && h.hidden[0].first == OLMBLUR_SMOOTHNESS && h.hidden[0].second,
          "Smoothness hidden");
}

void TestCurrent(int depth) {
    struct Case { float amount; int repeat, bias, ds_den; };
    const Case cases[] = {{5, 2, 1, 1}, {5, 2, 2, 1}, {9.5f, 4, 1, 1}, {9.5f, 4, 2, 2},
                          {2, 3, 1, 1}, {1.25f, 1, 2, 1}, {40, 10, 1, 1}};
    for (const Case &cs : cases) {
        Host h;
        Init(h, depth, 23, 17);
        FillScene(h);
        h.in.downsample_x = {1, (A_u_long)cs.ds_den};
        h.params[OLMBLUR_LEGACY].u.bd.value = 0;
        h.params[OLMBLUR_AMOUNT].u.fs_d.value = cs.amount;
        h.params[OLMBLUR_REPEAT].u.sd.value = cs.repeat;
        h.params[OLMBLUR_BIAS].u.pd.value = cs.bias;
        CHECK(Render(h) == PF_Err_NONE, "render");
        std::vector<float> ref = Reference(h, cs.amount, cs.repeat, cs.bias, 1.0f / cs.ds_den);
        bool exact = true, alpha = true, gap_kept = true, no_bleed = true;
        for (int y = 0; y < h.h; ++y)
            for (int x = 0; x < h.w; ++x) {
                Px o = Get(h, h.output, x, y), in = Get(h, h.input, x, y);
                const float *r = &ref[3 * (y * h.w + x)];
                exact &= o.r == Quant(h, r[0]) && o.g == Quant(h, r[1]) && o.b == Quant(h, r[2]);
                alpha &= o.a == in.a;
                if (x == h.w / 2) gap_kept &= o.r == in.r && o.g == in.g && o.b == in.b;
                else if (x < h.w / 2) no_bleed &= o.b == 0;   // right side's blue never leaks left
                else no_bleed &= o.r == 0;                    // left side's red never leaks right
            }
        CHECK(exact, "matches brute-force reference");
        CHECK(alpha, "alpha untouched");
        CHECK(gap_kept, "transparent pixels keep their RGB");
        CHECK(no_bleed, "no bleed across the transparent division");
    }
    // Amount 0 = pass-through (binary early-out after utils->copy).
    Host h;
    Init(h, depth, 9, 7);
    FillScene(h);
    h.params[OLMBLUR_LEGACY].u.bd.value = 0;
    h.params[OLMBLUR_AMOUNT].u.fs_d.value = 0;
    Render(h);
    CHECK(!std::memcmp(h.in_bytes.data(), h.out_bytes.data(), h.in_bytes.size()), "amount 0 copies input");
    CHECK(h.layer_out == h.layer_in && h.checkouts == h.checkins, "checkouts balanced");
}

void TestLegacy(int depth) {
    const float M = depth == 8 ? 255.0f : depth == 16 ? 32768.0f : 1.0f;
    Host h;
    // Amount 3, Repeat 2 (default): two passes of radius 3 reach 6 px.
    Init(h, depth, 24, 6);
    const float half = depth == 32 ? 0.5f : (float)(int)(M * 0.5f);
    for (int y = 0; y < h.h; ++y)
        for (int x = 0; x < h.w; ++x) Put(h, x, y, {M, x < 12 ? M : 0.0f, half, 0});
    h.params[OLMBLUR_LEGACY].u.bd.value = 1;  // value for old projects
    h.params[OLMBLUR_AMOUNT].u.fs_d.value = 3;
    CHECK(Render(h) == PF_Err_NONE, "legacy render");
    Px edge_l = Get(h, h.output, 11, 3), edge_r = Get(h, h.output, 12, 3);
    Px far_l = Get(h, h.output, 3, 3), far_r = Get(h, h.output, 20, 3);
    const float tol = depth == 32 ? 1e-6f : 0.0f;  // float averages may round
    CHECK(edge_l.r < M && edge_l.r > 0 && edge_r.r > 0 && edge_r.r < M, "legacy blurs across a colour edge");
    CHECK(Get(h, h.output, 6, 3).r < M - tol && std::fabs(Get(h, h.output, 5, 3).r - M) <= tol,
          "legacy reach is 6 px for Repeat 2");
    CHECK(std::fabs(far_l.r - M) <= tol && far_r.r == 0, "legacy leaves flat areas exact");
    CHECK(std::fabs(edge_l.g - half) <= tol, "legacy flat channel stays put");
    // Column 0 is never a tap: x=0's value comes only from x=1..R.
    Host h2;
    Init(h2, depth, 8, 1);
    for (int x = 0; x < 8; ++x) Put(h2, x, 0, {M, x == 0 ? M : 0.0f, 0, 0});
    h2.params[OLMBLUR_LEGACY].u.bd.value = 1;
    h2.params[OLMBLUR_AMOUNT].u.fs_d.value = 2;
    Render(h2);
    CHECK(Get(h2, h2.output, 0, 0).r == 0, "legacy: column 0 excluded from its own taps");
    CHECK(Get(h2, h2.output, 1, 0).r == 0, "legacy: column 0 never contributes to neighbours");
}

}  // namespace

int main() {
    TestSetup();
    for (int depth : {8, 16, 32}) {
        TestCurrent(depth);
        TestLegacy(depth);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

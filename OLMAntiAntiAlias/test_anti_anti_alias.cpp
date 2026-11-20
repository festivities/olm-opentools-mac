// Fake-host test for OLMAntiAntiAlias. Includes the port source and runs the production EffectMain
// through PARAMS_SETUP, GLOBAL_SETUP/ABOUT and SMART_RENDER at 8/16/32 bpc.
// Build (Windows, MinGW, Win 26.5 SDK): see .agents/AGENTS.md "Test/build recipes".

#include "OLMAntiAntiAlias.cpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                 \
    do {                                                 \
        if (cond) {                                      \
            ++g_pass;                                    \
        } else {                                         \
            ++g_fail;                                    \
            std::printf("FAIL line %d: ", __LINE__);     \
            std::printf(__VA_ARGS__);                    \
            std::printf("\n");                           \
        }                                                \
    } while (0)

// ---------------------------------------------------------------- fake host ---
PF_ParamDef g_params[OLMAAA_NUM_PARAMS];
bool g_added[OLMAAA_NUM_PARAMS];
int g_add_calls = 0;

PF_Err FakeAddParam(PF_ProgPtr, PF_ParamIndex index, PF_ParamDef *def) {
    if (!def) return PF_Err_BAD_CALLBACK_PARAM;
    const int pos = (int)def->uu.id;
    (void)index;
    if (pos < 0 || pos >= OLMAAA_NUM_PARAMS) return PF_Err_BAD_CALLBACK_PARAM;
    g_params[pos] = *def;
    g_added[pos] = true;
    ++g_add_calls;
    return PF_Err_NONE;
}

PF_Err FakeCheckoutParam(PF_ProgPtr, PF_ParamIndex index, A_long, A_long, A_u_long, PF_ParamDef *def) {
    if (index < 0 || index >= OLMAAA_NUM_PARAMS || !def) return PF_Err_BAD_CALLBACK_PARAM;
    *def = g_params[index];
    return PF_Err_NONE;
}

PF_Err FakeCheckinParam(PF_ProgPtr, PF_ParamDef *) { return PF_Err_NONE; }

PF_Err FakeCopy(PF_ProgPtr, PF_EffectWorld *src, PF_EffectWorld *dst, PF_Rect *, PF_Rect *) {
    const int rows = src->height < dst->height ? src->height : dst->height;
    const size_t bytes = (size_t)(src->rowbytes < dst->rowbytes ? src->rowbytes : dst->rowbytes);
    for (int y = 0; y < rows; ++y)
        std::memcpy((char *)dst->data + (size_t)y * dst->rowbytes,
                    (const char *)src->data + (size_t)y * src->rowbytes, bytes);
    return PF_Err_NONE;
}

PF_EffectWorld *g_in_world = nullptr, *g_out_world = nullptr;
PF_Err FakeCheckoutLayerPixels(PF_ProgPtr, A_long, PF_EffectWorld **w) {
    *w = g_in_world;
    return PF_Err_NONE;
}
PF_Err FakeCheckoutOutput(PF_ProgPtr, PF_EffectWorld **w) {
    *w = g_out_world;
    return PF_Err_NONE;
}

PF_Err FakeCheckoutLayer(PF_ProgPtr, A_long, A_long, PF_RenderRequest *, A_long, A_long, A_u_long,
                         PF_CheckoutResult *res) {
    res->result_rect = PF_Rect{0, 0, 8, 8};
    res->max_result_rect = PF_Rect{0, 0, 8, 8};
    return PF_Err_NONE;
}

// Minimal SPBasic: AcquireSuite hands out the colour-param suite only (render colours).
PF_ColorParamSuite1 g_color_suite;
PF_Err FakeGetFloatColor(PF_ProgPtr, const PF_ParamDef *def, PF_PixelFloat *c) {
    c->alpha = def->u.cd.value.alpha / 255.0f;
    c->red = def->u.cd.value.red / 255.0f;
    c->green = def->u.cd.value.green / 255.0f;
    c->blue = def->u.cd.value.blue / 255.0f;
    return PF_Err_NONE;
}
SPErr FakeAcquire(const char *name, int32 version, const void **suite) {
    (void)version;
    if (std::strcmp(name, kPFColorParamSuite) == 0) {
        g_color_suite.PF_GetFloatingPointColorFromColorDef = FakeGetFloatColor;
        *suite = &g_color_suite;
        return kSPNoError;
    }
    *suite = nullptr;
    return kSPSuiteNotFoundError;
}
SPErr FakeRelease(const char *, int32) { return kSPNoError; }
SPBasicSuite g_basic{};

void SetupHost(PF_InData &in, PF_OutData &out, PF_UtilCallbacks &utils) {
    std::memset(&in, 0, sizeof(in));
    std::memset(&out, 0, sizeof(out));
    std::memset(&utils, 0, sizeof(utils));
    utils.copy = FakeCopy;
    g_basic.AcquireSuite = FakeAcquire;
    g_basic.ReleaseSuite = FakeRelease;
    in.inter.add_param = FakeAddParam;
    in.inter.checkout_param = FakeCheckoutParam;
    in.inter.checkin_param = FakeCheckinParam;
    in.utils = &utils;
    in.pica_basicP = nullptr;  // GLOBAL_SETUP skips AEGP registration without a pica
    in.effect_ref = (PF_ProgPtr)1;
    in.current_time = 0;
    in.time_step = 1;
    in.time_scale = 1;
    std::memset(g_params, 0, sizeof(g_params));
    std::memset(g_added, 0, sizeof(g_added));
    g_add_calls = 0;
}

// -------------------------------------------------------------- checks ---
void TestIdentityAndSetup() {
    PF_InData in;
    PF_OutData out;
    PF_UtilCallbacks utils;
    SetupHost(in, out, utils);

    EffectMain(PF_Cmd_GLOBAL_SETUP, &in, &out, nullptr, nullptr, nullptr);
    CHECK(out.my_version == 569344, "effect version %ld != 569344 (1.1.6)", (long)out.my_version);
    CHECK((out.out_flags2 & 0x08001400) == 0x08001400, "out_flags2 missing SMART|FLOAT|THREADED");
    CHECK((out.out_flags & 0x02000040) == 0x02000040, "out_flags missing USE_OUTPUT_EXTENT|DEEP");

    SetupHost(in, out, utils);
    EffectMain(PF_Cmd_ABOUT, &in, &out, nullptr, nullptr, nullptr);
    CHECK(std::strcmp(out.return_msg, "OLM Anti Anti Alias v1.1.6\rGet rid of anti-aliasing") == 0,
          "about text '%s'", out.return_msg);

    SetupHost(in, out, utils);
    EffectMain(PF_Cmd_PARAMS_SETUP, &in, &out, nullptr, nullptr, nullptr);
    CHECK(out.num_params == OLMAAA_NUM_PARAMS, "num_params %ld != 108", (long)out.num_params);
    CHECK(g_add_calls == 107, "add_param calls %d != 107", g_add_calls);

    const PF_ParamDef &count = g_params[OLMAAA_NUM_COLORS];
    CHECK(count.param_type == PF_Param_SLIDER, "count type");
    CHECK(count.u.sd.valid_min == 0 && count.u.sd.valid_max == 100, "count valid range");
    CHECK(count.u.sd.dephault == 0, "count default");
    CHECK(g_params[OLMAAA_USE_AUTO].param_type == PF_Param_CHECKBOX, "auto type");
    CHECK(g_params[OLMAAA_INITIALIZE].param_type == PF_Param_BUTTON, "init button type");
    CHECK(g_params[OLMAAA_INITIALIZE].flags == PF_ParamFlag_SUPERVISE, "button supervise");
    CHECK(g_params[OLMAAA_COLORS_GROUP].param_type == PF_Param_GROUP_START, "group start");
    CHECK(g_params[OLMAAA_COLOR_FIRST].param_type == PF_Param_COLOR, "first colour type");
    CHECK(g_params[OLMAAA_COLOR_FIRST + 99].param_type == PF_Param_COLOR, "last colour type");
    CHECK(g_params[OLMAAA_COLORS_GROUP_END].param_type == PF_Param_GROUP_END, "group end");
    CHECK(std::strcmp(g_params[OLMAAA_COLOR_FIRST].PF_DEF_NAME, "Color 1") == 0, "first colour name");
}

// Fills a w*h 8-bit ARGB image (PF order) from fn(x,y).
template <class F>
void FillImage(std::vector<typename F::T> &buf, int w, int h, int rowbytes_pixels,
               AAAColor (*fn)(int, int)) {
    (void)rowbytes_pixels;
    buf.assign((size_t)w * h, typename F::T{});
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const AAAColor c = fn(x, y);
            F::write(buf[(size_t)y * w + x], c);
        }
}

AAAColor Pattern(int x, int y) {
    (void)y;
    if (x < 3) return AAAColor{1.0f, 0.0f, 0.0f, 0.0f};    // black
    if (x > 4) return AAAColor{1.0f, 1.0f, 1.0f, 1.0f};    // white
    return AAAColor{1.0f, 0.5f, 0.5f, 0.5f};               // anti-aliased grey column
}

// Runs the SMART_RENDER path for one bit depth and returns the output buffer as floats.
template <class F>
void RenderPattern(int count, bool useAuto, const AAAColor *colours, std::vector<AAAColor> &result,
                   AAAColor (*fn)(int, int), int w, int h) {
    PF_InData in;
    PF_OutData out;
    PF_UtilCallbacks utils;
    SetupHost(in, out, utils);
    EffectMain(PF_Cmd_PARAMS_SETUP, &in, &out, nullptr, nullptr, nullptr);
    g_params[OLMAAA_NUM_COLORS].u.sd.value = count;
    g_params[OLMAAA_USE_AUTO].u.bd.value = useAuto ? 1 : 0;
    for (int i = 0; i < count; ++i) {
        g_params[OLMAAA_COLOR_FIRST + i].u.cd.value.alpha = (A_u_char)(colours[i].a * 255.0f);
        g_params[OLMAAA_COLOR_FIRST + i].u.cd.value.red = (A_u_char)(colours[i].r * 255.0f);
        g_params[OLMAAA_COLOR_FIRST + i].u.cd.value.green = (A_u_char)(colours[i].g * 255.0f);
        g_params[OLMAAA_COLOR_FIRST + i].u.cd.value.blue = (A_u_char)(colours[i].b * 255.0f);
    }

    std::vector<typename F::T> inBuf, outBuf;
    FillImage<F>(inBuf, w, h, w, fn);
    outBuf.assign((size_t)w * h, typename F::T{});

    PF_EffectWorld inW, outW;
    std::memset(&inW, 0, sizeof(inW));
    std::memset(&outW, 0, sizeof(outW));
    inW.data = (PF_Pixel8 *)inBuf.data();
    inW.rowbytes = w * (int)sizeof(typename F::T);
    inW.width = w;
    inW.height = h;
    outW = inW;
    outW.data = (PF_Pixel8 *)outBuf.data();
    outW.extent_hint = PF_Rect{0, 0, w, h};
    g_in_world = &inW;
    g_out_world = &outW;

    PF_SmartRenderExtra extra;
    std::memset(&extra, 0, sizeof(extra));
    PF_SmartRenderCallbacks cb;
    std::memset(&cb, 0, sizeof(cb));
    cb.checkout_layer_pixels = FakeCheckoutLayerPixels;
    cb.checkout_output = FakeCheckoutOutput;
    PF_SmartRenderInput sin;
    std::memset(&sin, 0, sizeof(sin));
    sin.bitdepth = F::kBpc;
    extra.cb = &cb;
    extra.input = &sin;
    in.pica_basicP = &g_basic;
    EffectMain(PF_Cmd_SMART_RENDER, &in, &out, nullptr, nullptr, &extra);
    in.pica_basicP = nullptr;

    result.resize((size_t)w * h);
    for (size_t i = 0; i < result.size(); ++i) result[i] = F::read(outBuf[i]);
}

bool SameColor(const AAAColor &a, const AAAColor &b) {
    return std::fabs(a.r - b.r) < 0.01f && std::fabs(a.g - b.g) < 0.01f && std::fabs(a.b - b.b) < 0.01f;
}

void TestRenderInvariants() {
    const AAAColor kPal[2] = {AAAColor{1, 0, 0, 0}, AAAColor{1, 1, 1, 1}};  // black, white
    const int W = 8, H = 8;
    const int kDepths[3] = {8, 16, 32};
    for (int di = 0; di < 3; ++di) {
        const int bpc = kDepths[di];
        std::vector<AAAColor> r;
        if (bpc == 8) RenderPattern<Pix8>(2, false, kPal, r, Pattern, W, H);
        if (bpc == 16) RenderPattern<Pix16>(2, false, kPal, r, Pattern, W, H);
        if (bpc == 32) RenderPattern<PixF>(2, false, kPal, r, Pattern, W, H);
        // hard black and white columns survive unchanged
        CHECK(SameColor(r[0 * W + 0], kPal[0]) && SameColor(r[3 * W + 1], kPal[0]), "bpc %d black kept", bpc);
        CHECK(SameColor(r[0 * W + 7], kPal[1]) && SameColor(r[5 * W + 6], kPal[1]), "bpc %d white kept", bpc);
        // the grey boundary column becomes one of the palette colours
        bool onPalette = true;
        for (int y = 0; y < H; ++y) {
            const AAAColor &g = r[y * W + 3];
            if (!(SameColor(g, kPal[0]) || SameColor(g, kPal[1]))) onPalette = false;
        }
        CHECK(onPalette, "bpc %d grey column snaps to palette", bpc);
    }
}

void TestPassThrough() {
    std::vector<AAAColor> r;
    RenderPattern<Pix8>(0, false, nullptr, r, Pattern, 8, 8);
    CHECK(SameColor(r[0 * 8 + 3], AAAColor{1, 0.5f, 0.5f, 0.5f}), "zero colours pass through");
}

void TestPairCostSymmetry() {
    // a pixel exactly halfway between two palette colours has a near-zero blend cost
    Palette p;
    p.n = 2;
    p.col = {AAAColor{1, 0, 0, 0}, AAAColor{1, 1, 1, 1}};
    p.BuildPairs();
    const AAAColor P{1, 0.5f, 0.5f, 0.5f};
    int first = -1, second = -1;
    BestPair(p, P, std::vector<int>{0, 1}, first, second);
    CHECK(first >= 0 && second >= 0 && first != second, "pair picks two distinct colours");

    // Identical palette colours: the pair direction is zero, the cost is NaN (0/0) and is never
    // accepted (binary behaviour, e2e-verified). Such candidates must not produce a pick.
    Palette dup;
    dup.n = 2;
    dup.col = {AAAColor{1, 0.5f, 0.5f, 0.5f}, AAAColor{1, 0.5f, 0.5f, 0.5f}};
    dup.BuildPairs();
    int f2 = 0, s2 = 0;
    BestPair(dup, AAAColor{1, 0.4f, 0.4f, 0.4f}, std::vector<int>{0, 1}, f2, s2);
    CHECK(f2 == -1 && s2 == -1, "duplicate palette pair is never picked");
    SinglePair(dup, AAAColor{1, 0.4f, 0.4f, 0.4f}, 0, f2, s2);
    CHECK(f2 == -1, "duplicate single-candidate partner is never picked");
}

// Count/slot rules of the USER_CHANGED add path are covered by the e2e harness (294 UI cases);
// the unit test only checks the pure pair fit. See the spec for the rules.

}  // namespace

int main() {
    TestIdentityAndSetup();
    TestPassThrough();
    TestRenderInvariants();
    TestPairCostSymmetry();
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

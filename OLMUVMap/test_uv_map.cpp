// Fake AE host; production EffectMain runs GLOBAL_SETUP -> PARAMS_SETUP -> SEQUENCE_SETUP ->
// UPDATE_PARAMS_UI -> SMART_PRE_RENDER -> SMART_RENDER. Covers identities and flags, the
// param definitions, the Smoothness UI rule, pre-render union, nearest/bilinear/trilinear at
// 8/16/32 bpc, the mip chain (even and odd sizes), the alpha guard and the missing-mip error.
#include "OLMUVMap.cpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
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

struct Block {
    void *data;
};

struct Host {
    int depth = 8;
    SPBasicSuite basic{};
    PF_UtilCallbacks utils{};
    PF_InData in{};
    PF_OutData out{};
    std::vector<unsigned char> in_bytes, tex_bytes, out_bytes;
    PF_EffectWorld input{}, texture{}, output{};
    PF_ParamDef params[OLMUV_NUM_PARAMS]{};
    int add_count = 0;
    std::vector<std::pair<PF_ParamIndex, bool>> ui_log;  // UpdateParamUI records
    PF_ParamUtilsSuite3 utils3{};
    std::vector<Block *> blocks;  // leaked on purpose: the test is short-lived
};
Host *g = nullptr;

PF_Err add_param(PF_ProgPtr, PF_ParamIndex, PF_ParamDefPtr def) {
    if (g->add_count >= OLMUV_NUM_PARAMS - 1) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    g->params[++g->add_count] = *def;
    return PF_Err_NONE;
}
PF_Err checkout_param(PF_ProgPtr, PF_ParamIndex i, A_long, A_long, A_u_long, PF_ParamDef *p) {
    if (i <= 0 || i >= OLMUV_NUM_PARAMS) return PF_Err_BAD_CALLBACK_PARAM;
    *p = g->params[i];
    return PF_Err_NONE;
}
PF_Err checkin_param(PF_ProgPtr, PF_ParamDef *) { return PF_Err_NONE; }
int SPAPI mock_sprintf(A_char *buf, const A_char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    const int n = std::vsnprintf(buf, 256, fmt, a);
    va_end(a);
    return n;
}
PF_Err SPAPI update_ui(PF_ProgPtr, PF_ParamIndex i, const PF_ParamDef *d) {
    if (d->param_type != g->params[i].param_type) return PF_Err_BAD_CALLBACK_PARAM;
    g->ui_log.push_back({i, (d->ui_flags & PF_PUI_DISABLED) != 0});
    return PF_Err_NONE;
}
SPErr SPAPI acquire(const char *n, int32 v, const void **s) {
    if (!std::strcmp(n, kPFParamUtilsSuite) && v == kPFParamUtilsSuiteVersion3) *s = &g->utils3;
    else return (SPErr)-1;
    return kSPNoError;
}
SPErr SPAPI release(const char *, int32) { return kSPNoError; }

PF_Handle fake_new(A_u_longlong size) {
    auto *b = new Block{std::calloc(1, (size_t)size)};
    g->blocks.push_back(b);
    return (PF_Handle)b;
}
void *fake_lock(PF_Handle h) { return ((Block *)h)->data; }
void fake_unlock(PF_Handle) {}
void fake_dispose(PF_Handle h) {
    std::free(((Block *)h)->data);
    ((Block *)h)->data = nullptr;
}

PF_Err co_layer(PF_ProgPtr, A_long, PF_EffectWorld **w) { *w = &g->input; return PF_Err_NONE; }
PF_Err ci_layer(PF_ProgPtr, A_long) { return PF_Err_NONE; }
PF_Err co_output(PF_ProgPtr, PF_EffectWorld **w) { *w = &g->output; return PF_Err_NONE; }
PF_Err pre_checkout(PF_ProgPtr, PF_ParamIndex, A_long, const PF_RenderRequest *r, A_long, A_long,
                    A_u_long, PF_CheckoutResult *res) {
    res->result_rect = res->max_result_rect = r->rect;
    return PF_Err_NONE;
}

size_t PixSize(int d) { return d == 8 ? 4 : d == 16 ? 8 : 16; }

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

// Normalised value v in 0..1 -> stored value for the depth (8: round(v*255), 16: round(v*32768)).
double Store(int d, double v) {
    if (d == 8) return std::floor(v * 255.0 + 0.5);
    if (d == 16) return std::floor(v * 32768.0 + 0.5);
    return v;
}
// Writes normalised channels (A,R,G,B in 0..1) to a world pixel.
void PutN(PF_EffectWorld &w, int d, int x, int y, double a, double r, double g, double b) {
    char *row = (char *)w.data + (size_t)y * w.rowbytes;
    if (d == 8) {
        PF_Pixel *p = ((PF_Pixel *)row) + x;
        p->alpha = (A_u_char)Store(d, a); p->red = (A_u_char)Store(d, r);
        p->green = (A_u_char)Store(d, g); p->blue = (A_u_char)Store(d, b);
    } else if (d == 16) {
        PF_Pixel16 *p = ((PF_Pixel16 *)row) + x;
        p->alpha = (A_u_short)Store(d, a); p->red = (A_u_short)Store(d, r);
        p->green = (A_u_short)Store(d, g); p->blue = (A_u_short)Store(d, b);
    } else {
        PF_PixelFloat *p = ((PF_PixelFloat *)row) + x;
        p->alpha = (float)a; p->red = (float)r; p->green = (float)g; p->blue = (float)b;
    }
}
// Reads a pixel as raw stored values (A,R,G,B); for 32 bpc the floats are returned as-is.
void ReadRaw(int d, const PF_EffectWorld &w, int x, int y, double out[4]) {
    const char *row = (const char *)w.data + (size_t)y * w.rowbytes;
    if (d == 8) {
        PF_Pixel p = ((const PF_Pixel *)row)[x];
        out[0] = p.alpha; out[1] = p.red; out[2] = p.green; out[3] = p.blue;
    } else if (d == 16) {
        PF_Pixel16 p = ((const PF_Pixel16 *)row)[x];
        out[0] = p.alpha; out[1] = p.red; out[2] = p.green; out[3] = p.blue;
    } else {
        PF_PixelFloat p = ((const PF_PixelFloat *)row)[x];
        out[0] = p.alpha; out[1] = p.red; out[2] = p.green; out[3] = p.blue;
    }
}

void Init(Host &h, int d, int tw, int th) {
    g = &h;
    h.depth = d;
    SetWorld(h.input, h.in_bytes, tw, th, d);
    SetWorld(h.output, h.out_bytes, tw, th, d);
    SetWorld(h.texture, h.tex_bytes, tw, th, d);
    h.utils3.PF_UpdateParamUI = update_ui;
    h.basic.AcquireSuite = acquire;
    h.basic.ReleaseSuite = release;
    h.utils.ansi.sprintf = mock_sprintf;
    h.utils.host_new_handle = fake_new;
    h.utils.host_lock_handle = fake_lock;
    h.utils.host_unlock_handle = fake_unlock;
    h.utils.host_dispose_handle = fake_dispose;
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
    EffectMain(PF_Cmd_SEQUENCE_SETUP, &h.in, &h.out, nullptr, nullptr, nullptr);
    h.in.sequence_data = h.out.sequence_data;  // AE mirrors the handle into in_data
    h.params[OLMUV_INTERP].u.pd.value = OLMUV_TRILINEAR;
    h.params[OLMUV_SMOOTHNESS].u.fs_d.value = 0.5;
}

// Runs PRE_RENDER and SMART_RENDER with the given popup mode and Smoothness value.
PF_Err Render(Host &h, int mode, double smooth) {
    h.params[OLMUV_INTERP].u.pd.value = mode;
    h.params[OLMUV_SMOOTHNESS].u.fs_d.value = smooth;
    h.params[OLMUV_TEXTURE].u.ld = h.texture;
    PF_PreRenderInput pin{};
    PF_PreRenderOutput pout{};
    PF_PreRenderCallbacks pcb{};
    PF_PreRenderExtra pex{};
    pin.output_request.rect = {0, 0, h.input.width, h.input.height};
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

// Texture 4x4: texel (x,y) = A 1.0, R 60x/255, G 60y/255, B 20(x+y)/255 (normalised).
void FillTexture4x4(Host &h) {
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            PutN(h.texture, h.depth, x, y, 1.0, 60.0 * x / 255.0, 60.0 * y / 255.0,
                 20.0 * (x + y) / 255.0);
}

// Input: every pixel has UV (rn, gn) and alpha an; B unused.
void FillInput(Host &h, int w, int hgt, double rn, double gn, double an) {
    for (int y = 0; y < hgt; ++y)
        for (int x = 0; x < w; ++x) PutN(h.input, h.depth, x, y, an, rn, gn, 0.0);
}

}  // namespace

int main() {
    // ---- identity and flags ----
    {
        Host h;
        Init(h, 8, 4, 4);
        CHECK(h.out.my_version == 559104, "my_version = 1.1.1");
        CHECK(h.out.out_flags == 0x06000040, "out_flags = binary 0x02000040 + SEND_UPDATE_PARAMS_UI");
        CHECK(h.out.out_flags2 == 0x18001400, "out_flags2 = binary");
        EffectMain(PF_Cmd_ABOUT, &h.in, &h.out, nullptr, nullptr, nullptr);
        CHECK(std::strcmp(h.out.return_msg, "OLM UV Map v1.1.1\rMove pixels around in an image") == 0,
              "About string");
        CHECK(h.out.num_params == OLMUV_NUM_PARAMS, "num_params = 4");
    }

    // ---- param definitions ----
    {
        Host h;
        Init(h, 8, 4, 4);
        CHECK(h.params[OLMUV_TEXTURE].param_type == PF_Param_LAYER, "Texture is LAYER");
        CHECK(h.params[OLMUV_TEXTURE].uu.id == 1, "Texture id 1");
        CHECK(std::strcmp(h.params[OLMUV_TEXTURE].PF_DEF_NAME, "Texture") == 0, "Texture name");
        CHECK(h.params[OLMUV_INTERP].param_type == PF_Param_POPUP, "Interp is POPUP");
        CHECK(h.params[OLMUV_INTERP].uu.id == 2, "Interp id 2");
        CHECK(h.params[OLMUV_INTERP].u.pd.num_choices == 3, "Interp 3 choices");
        CHECK(h.params[OLMUV_INTERP].u.pd.dephault == 3, "Interp default Trilinear");
        CHECK(std::strcmp(h.params[OLMUV_INTERP].PF_DEF_NAME, "Interpolation Method") == 0,
              "Interp name");
        CHECK(std::strcmp(h.params[OLMUV_INTERP].u.pd.u.PF_DEF_NAMESPTR,
                          "Nearest Neighbor|Bilinear|Trilinear") == 0,
              "Interp choices string");
        CHECK(h.params[OLMUV_SMOOTHNESS].param_type == PF_Param_FLOAT_SLIDER, "Smoothness slider");
        CHECK(h.params[OLMUV_SMOOTHNESS].uu.id == 3, "Smoothness id 3");
        CHECK(h.params[OLMUV_SMOOTHNESS].flags == PF_ParamFlag_SUPERVISE, "Smoothness SUPERVISE");
        CHECK(h.params[OLMUV_SMOOTHNESS].u.fs_d.valid_min == 0.0f &&
                  h.params[OLMUV_SMOOTHNESS].u.fs_d.valid_max == 1.0f,
              "Smoothness valid 0..1");
        CHECK(h.params[OLMUV_SMOOTHNESS].u.fs_d.dephault == 0.5f, "Smoothness default 0.5");
        CHECK(h.params[OLMUV_SMOOTHNESS].u.fs_d.precision == 2, "Smoothness precision 2");
        CHECK(h.params[OLMUV_SMOOTHNESS].u.fs_d.curve_tolerance == 3.0f, "curve tolerance 3.0");
    }

    // ---- Smoothness UI rule: disabled unless Trilinear ----
    {
        Host h;
        Init(h, 8, 4, 4);
        const int modes[3] = {OLMUV_NEAREST, OLMUV_BILINEAR, OLMUV_TRILINEAR};
        const bool expectDisabled[3] = {true, true, false};
        for (int i = 0; i < 3; ++i) {
            h.params[OLMUV_INTERP].u.pd.value = modes[i];
            h.ui_log.clear();
            EffectMain(PF_Cmd_UPDATE_PARAMS_UI, &h.in, &h.out, nullptr, nullptr, nullptr);
            CHECK(h.ui_log.size() == 1, "one UpdateParamUI per UI pass");
            if (h.ui_log.size() == 1) {
                CHECK(h.ui_log[0].first == OLMUV_SMOOTHNESS, "UpdateParamUI targets Smoothness");
                CHECK(h.ui_log[0].second == expectDisabled[i], "Smoothness disabled rule");
            }
        }
    }

    // ---- pre-render union ----
    {
        Host h;
        Init(h, 8, 4, 4);
        PF_PreRenderInput pin{};
        PF_PreRenderOutput pout{};
        PF_PreRenderCallbacks pcb{};
        PF_PreRenderExtra pex{};
        pin.output_request.rect = {1, 2, 3, 4};
        pcb.checkout_layer = pre_checkout;
        pex.input = &pin;
        pex.output = &pout;
        pex.cb = &pcb;
        CHECK(EffectMain(PF_Cmd_SMART_PRE_RENDER, &h.in, &h.out, nullptr, nullptr, &pex) == PF_Err_NONE,
              "pre-render ok");
        CHECK(pout.result_rect.left == 1 && pout.result_rect.top == 2 && pout.result_rect.right == 3 &&
                  pout.result_rect.bottom == 4,
              "pre-render result rect");
        CHECK(pout.max_result_rect.right == 3 && pout.max_result_rect.bottom == 4, "pre-render max rect");
    }

    // ---- nearest, 8/16/32: UV (u=128/255, v=0) hits texel (1,0) exactly ----
    for (int d : {8, 16, 32}) {
        Host h;
        Init(h, d, 4, 4);
        FillTexture4x4(h);
        // u = 128/255 -> X = 3 * 0.50196 = 1.506 -> texel column 1. G = 1 -> V = 0 -> row 0.
        FillInput(h, 4, 4, 128.0 / 255.0, 1.0, 1.0);
        CHECK(Render(h, OLMUV_NEAREST, 0.0) == PF_Err_NONE, "nearest render");
        double got[4];
        ReadRaw(d, h.output, 2, 2, got);
        double want[4];
        ReadRaw(d, h.texture, 1, 0, want);  // texel (1,0) raw, copied for RGB
        CHECK(got[1] == want[1] && got[2] == want[2] && got[3] == want[3], "nearest RGB = texel (1,0)");
        CHECK(got[0] == want[0], "nearest alpha = input alpha (1.0) * texel alpha");
        // Input alpha 0 -> whole output zero.
        FillInput(h, 4, 4, 0.5, 0.5, 0.0);
        CHECK(Render(h, OLMUV_NEAREST, 0.0) == PF_Err_NONE, "nearest render (alpha 0)");
        double z[4];
        ReadRaw(d, h.output, 1, 1, z);
        CHECK(z[0] == 0.0 && z[1] == 0.0 && z[2] == 0.0 && z[3] == 0.0, "alpha 0 -> zero output");
    }

    // ---- bilinear midpoint on a 2x1 texture, 8 bpc ----
    {
        Host h;
        Init(h, 8, 2, 1);
        PutN(h.texture, 8, 0, 0, 1.0, 0.0, 0.0, 0.0);
        PutN(h.texture, 8, 1, 0, 1.0, 1.0, 0.0, 0.0);
        FillInput(h, 2, 1, 128.0 / 255.0, 1.0, 1.0);
        CHECK(Render(h, OLMUV_BILINEAR, 0.0) == PF_Err_NONE, "bilinear render");
        double raw[4];
        ReadRaw(8, h.output, 0, 0, raw);
        // fx = 128/255 -> R = 255*fx = 128.0 (integer truncation of ~127.999 or 128.0).
        CHECK(raw[1] == 127 || raw[1] == 128, "bilinear midpoint R = 127 or 128");
        CHECK(raw[0] == 255, "bilinear alpha 255");
    }

    // ---- trilinear on a flat UV field equals bilinear on level 0 (32 bpc) ----
    {
        Host h;
        Init(h, 32, 4, 4);
        FillTexture4x4(h);
        FillInput(h, 4, 4, 0.3, 0.6, 1.0);
        CHECK(Render(h, OLMUV_TRILINEAR, 0.0) == PF_Err_NONE, "trilinear render");
        double tri[4];
        ReadRaw(32, h.output, 2, 2, tri);
        CHECK(Render(h, OLMUV_BILINEAR, 0.0) == PF_Err_NONE, "bilinear render (32)");
        double bil[4];
        ReadRaw(32, h.output, 2, 2, bil);
        bool same = true;
        for (int i = 0; i < 4; ++i) same = same && std::fabs(tri[i] - bil[i]) < 1e-5;
        CHECK(same, "trilinear(flat UV) == bilinear level 0");
        // Smoothness only matters with a varying UV footprint, so it must not change a flat field.
        CHECK(Render(h, OLMUV_TRILINEAR, 1.0) == PF_Err_NONE, "trilinear render (smooth 1)");
        double tri1[4];
        ReadRaw(32, h.output, 2, 2, tri1);
        bool same1 = true;
        for (int i = 0; i < 4; ++i) same1 = same1 && std::fabs(tri1[i] - bil[i]) < 1e-5;
        CHECK(same1, "smoothness does not change a flat UV field");
    }

    // ---- mip chain: even 4x4 box average, odd 3x3 mean ----
    {
        Host h;
        Init(h, 32, 4, 4);
        FillTexture4x4(h);
        const auto mips = BuildMips<32>(ViewOf(&h.texture));
        CHECK(mips.size() == 3, "4x4 has levels 0..2");
        // Level 1 (0,0): mean of texels (0..1, 0..1): R = 30/255, G = 30/255, B = 20/255.
        CHECK(std::fabs(mips[1].px[1] - 30.0f / 255.0f) < 1e-6f, "level1 R = mean");
        CHECK(std::fabs(mips[1].px[2] - 30.0f / 255.0f) < 1e-6f, "level1 G = mean");
        CHECK(std::fabs(mips[1].px[3] - 20.0f / 255.0f) < 1e-6f, "level1 B = mean");
        CHECK(std::fabs(mips[1].px[0] - 1.0f) < 1e-6f, "level1 A = 1");
        // Level 2 (single texel) = mean of all 16 texels: R = 90/255, G = 90/255, B = 60/255.
        CHECK(std::fabs(mips[2].px[1] - 90.0f / 255.0f) < 1e-6f, "level2 R = mean(60x)");
        CHECK(std::fabs(mips[2].px[2] - 90.0f / 255.0f) < 1e-6f, "level2 G = mean(60y)");
        CHECK(std::fabs(mips[2].px[3] - 60.0f / 255.0f) < 1e-6f, "level2 B = mean(20(x+y))");
    }
    {
        Host h;
        Init(h, 32, 3, 3);
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x) PutN(h.texture, 32, x, y, 1.0, (x + 3 * y) / 10.0, 0.0, 0.0);
        const auto odd = BuildMips<32>(ViewOf(&h.texture));
        CHECK(odd.size() == 2, "3x3 has levels 0..1");
        // Mean of 0.0 .. 0.8 (step 0.1) over the nine texels = 0.4.
        CHECK(std::fabs(odd[1].px[1] - 0.4f) < 1e-5f, "odd 3x3 -> 1x1 mean");
    }

    // ---- missing mip chain: error and message ----
    {
        Host h;
        Init(h, 8, 4, 4);
        FillTexture4x4(h);
        FillInput(h, 4, 4, 0.5, 0.5, 1.0);
        h.in.sequence_data = nullptr;
        h.out.sequence_data = nullptr;
        const PF_Err err = Render(h, OLMUV_TRILINEAR, 0.5);
        CHECK(err == PF_Err_INTERNAL_STRUCT_DAMAGED, "missing mips -> error");
        CHECK(std::strcmp(h.out.return_msg, "Trilinear invoked without mipmaps") == 0, "error message");
        CHECK((h.out.out_flags & PF_OutFlag_DISPLAY_ERROR_MESSAGE) != 0, "display error flag");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

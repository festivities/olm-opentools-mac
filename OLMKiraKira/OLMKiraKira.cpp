/* OLM Kira Kira: macOS port of OLMKiraKira.aex 3.3 (entry 0x1811560A0).
 * Streaks are a rotate / 1D blur / rotate-back of a transfer-function mask.
 * OpenCV calls are reimplemented. Ramp editor drawing is not ported; the
 * 324-byte stop list and 325-byte flatten (version byte + blob) are. */

#include "OLMKiraKira.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

static_assert(PF_VERSION(OLMKK_MAJOR_VERSION, OLMKK_MINOR_VERSION, OLMKK_BUG_VERSION,
                         PF_Stage_DEVELOP, 0) == 0x00198000,
              "Kira Kira version must match its PiPL");

// Port adds SEND_UPDATE_PARAMS_UI so Use Ramp can grey the color/ramp pair.
// Binary flags are 0x02008040; the disable code exists but AE never calls it.
constexpr PF_OutFlags kOutFlags = PF_OutFlag_USE_OUTPUT_EXTENT | PF_OutFlag_CUSTOM_UI |
                                  PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_SEND_UPDATE_PARAMS_UI;
constexpr PF_OutFlags2 kOutFlags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |
                                    PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
static_assert(kOutFlags == 0x06008040, "out_flags");
static_assert(kOutFlags2 == 0x08001400, "out_flags2");

constexpr int kArbBytes = 608;
constexpr int kRampOff = 16;
constexpr int kRampBytes = 324;
constexpr int kFlatBytes = 325;
constexpr int kStops = 16;

AEGP_PluginID g_aegp_id = 0;

struct Stop {
    float x, a, r, g, b;
};
struct Ramp {
    int count;
    Stop s[kStops];
};
static_assert(sizeof(Ramp) == kRampBytes, "ramp blob");

struct Arb {
    char head[kRampOff];
    Ramp ramp;
    char tail[kArbBytes - kRampOff - kRampBytes];
};
static_assert(sizeof(Arb) == kArbBytes, "arb");

Ramp DefaultRamp() {
    Ramp r{};
    r.count = 3;
    r.s[0] = {0.f, 1.f, 1.f, 0.f, 0.f};
    r.s[1] = {0.78f, 1.f, 1.f, 0.651f, 0.f};
    r.s[2] = {1.f, 1.f, 1.f, 1.f, 1.f};
    return r;
}

PF_Handle NewArb(PF_InData *in_data, const Ramp &ramp) {
    PF_Handle h = in_data->utils->host_new_handle(kArbBytes);
    if (!h) return nullptr;
    auto *a = (Arb *)in_data->utils->host_lock_handle(h);
    if (!a) return nullptr;
    std::memset(a, 0, sizeof(Arb));
    a->ramp = ramp;
    in_data->utils->host_unlock_handle(h);
    return h;
}

Ramp *LockRamp(PF_InData *in_data, PF_Handle h) {
    if (!h) return nullptr;
    auto *a = (Arb *)in_data->utils->host_lock_handle(h);
    return a ? &a->ramp : nullptr;
}

float Clamp01(float x) { return x >= 1.f ? 1.f : std::max(x, 0.f); }

void EvalRamp(const Ramp &ramp, float t, float c[4]) {
    if (ramp.count <= 0) {
        c[0] = 1.f;
        c[1] = c[2] = c[3] = 0.f;
        return;
    }
    const Stop *lo = nullptr;
    const Stop *hi = nullptr;
    const int n = std::min(ramp.count, kStops);
    for (int i = 0; i < n; ++i) {
        const Stop &s = ramp.s[i];
        if (s.x - t <= 0.f) {
            if (!lo || s.x > lo->x) lo = &s;
        } else if (!hi || s.x < hi->x) {
            hi = &s;
        }
    }
    auto put = [&](const Stop &s) {
        c[0] = s.a;
        c[1] = s.r;
        c[2] = s.g;
        c[3] = s.b;
    };
    if (lo && hi && std::fabs(hi->x - lo->x) >= 1e-4f) {
        const float f = (hi->x - t) / (hi->x - lo->x);
        c[0] = lo->a * f + hi->a * (1.f - f);
        c[1] = lo->r * f + hi->r * (1.f - f);
        c[2] = lo->g * f + hi->g * (1.f - f);
        c[3] = lo->b * f + hi->b * (1.f - f);
    } else if (lo) {
        put(*lo);
    } else if (hi) {
        put(*hi);
    } else {
        c[0] = 1.f;
        c[1] = c[2] = c[3] = 0.f;
    }
}

PF_Err About(PF_InData *in_data, PF_OutData *out_data) {
    PF_SPRINTF(out_data->return_msg, "%s", OLMKK_ABOUT);
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData *in_data, PF_OutData *out_data) {
    out_data->my_version = PF_VERSION(OLMKK_MAJOR_VERSION, OLMKK_MINOR_VERSION, OLMKK_BUG_VERSION,
                                      PF_Stage_DEVELOP, 0);
    out_data->out_flags = kOutFlags;
    out_data->out_flags2 = kOutFlags2;
    if (in_data->pica_basicP) {
        AEGP_SuiteHandler suites(in_data->pica_basicP);
        suites.UtilitySuite3()->AEGP_RegisterWithAEGP(nullptr, OLMKK_NAME, &g_aegp_id);
    }
    return PF_Err_NONE;
}

PF_Err AddPopup(PF_InData *in_data, PF_ParamDef &def, const char *name, int choices, int dflt,
               const char *items, PF_ParamFlags flags, int id) {
    AEFX_CLR_STRUCT(def);
    def.flags = flags;
    PF_ADD_POPUP(name, choices, dflt, items, id);
    return PF_Err_NONE;
}

PF_Err AddSlider(PF_InData *in_data, PF_ParamDef &def, const char *name, int vmin, int vmax,
                int smin, int smax, int dflt, int id) {
    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER(name, vmin, vmax, smin, smax, dflt, id);
    return PF_Err_NONE;
}

PF_Err AddCheck(PF_InData *in_data, PF_ParamDef &def, const char *name, int id) {
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX(name, name, 0, 0, id);
    return PF_Err_NONE;
}

PF_Err AddColor(PF_InData *in_data, PF_ParamDef &def, const char *name, int id) {
    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR(name, 255, 255, 255, id);
    return PF_Err_NONE;
}

PF_Err AddRamp(PF_InData *in_data, PF_ParamDef &def, int id) {
    AEFX_CLR_STRUCT(def);
    PF_Handle h = NewArb(in_data, DefaultRamp());
    PF_ADD_ARBITRARY2("Ramp", 310, 170, PF_ParamFlag_COLLAPSE_TWIRLY | PF_ParamFlag_SUPERVISE,
                      PF_PUI_CONTROL | PF_PUI_DONT_ERASE_CONTROL, h, id, nullptr);
    return PF_Err_NONE;
}

PF_Err ParamsSetup(PF_InData *in_data, PF_OutData *out_data) {
    PF_ParamDef def;
    if (in_data->inter.register_ui) {
        PF_CustomUIInfo ui{};
        ui.events = PF_CustomEFlag_EFFECT;
        in_data->inter.register_ui(in_data->effect_ref, &ui);
    }
    AddPopup(in_data, def, "Channel", 6, 1, "Alpha|Luminance|RGB|Brightness",
             PF_ParamFlag_SUPERVISE, 8);
    AddPopup(in_data, def, "Blur Mode", 3, 2, "Box|Approximated Gaussian|Gaussian|Exponential",
             PF_ParamFlag_SUPERVISE, 9);
    AddPopup(in_data, def, "Merge mode", 2, 1, "premultiply|add",
             PF_ParamFlag_SUPERVISE | PF_ParamFlag_USE_VALUE_FOR_OLD_PROJECTS, 17);
    AddCheck(in_data, def, "Approximated Input", 10);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDER("Brightness Gain", 1, 100, 1, 100, 0, 0.1, 3, PF_ValueDisplayFlag_NONE,
                        false, 2);
    AddSlider(in_data, def, "Strength multiplier", 0, 1000, 0, 1000, 100, 11);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDER("Fade Out", 0, 1, 0, 1, 0, 0.0, 1, PF_ValueDisplayFlag_NONE, false, 27);
    AddSlider(in_data, def, "Glow Opacity", 0, 10000, 0, 10000, 100, 7);
    AddSlider(in_data, def, "Source Opacity", 0, 100, 0, 100, 100, 12);
    AddSlider(in_data, def, "Vertical Length", 0, 1000, 0, 200, 50, 3);
    AddColor(in_data, def, "Vertical Color", 13);
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Vertical Color Ramp", 29);
    AddCheck(in_data, def, "Use Ramp", 18);
    AddRamp(in_data, def, 19);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(30);
    AddSlider(in_data, def, "Horizontal Length", 0, 1000, 0, 200, 50, 4);
    AddColor(in_data, def, "Horizontal Color", 14);
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Horizontal Color Ramp", 31);
    AddCheck(in_data, def, "Use Ramp", 20);
    AddRamp(in_data, def, 21);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(32);
    AddSlider(in_data, def, "Diagonal Length", 0, 1000, 0, 200, 50, 5);
    AddColor(in_data, def, "Diagonal Color", 15);
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Diagonal Color Ramp", 33);
    AddCheck(in_data, def, "Use Ramp", 22);
    AddRamp(in_data, def, 23);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(34);
    AddSlider(in_data, def, "Diagonal 2 length", 0, 1000, 0, 200, 50, 26);
    AddColor(in_data, def, "Diagonal Color2", 28);
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Diagonal 2 Color Ramp", 37);
    AddCheck(in_data, def, "Use Ramp", 35);
    AddRamp(in_data, def, 36);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(38);
    AddSlider(in_data, def, "Highlight Radius", 0, 8000, 0, 200, 0, 6);
    AddColor(in_data, def, "Highlight Color", 16);
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Highlight Color Ramp", 39);
    AddCheck(in_data, def, "Use Ramp", 24);
    AddRamp(in_data, def, 25);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(40);
    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Glow Rotation", 0, 1);
    out_data->num_params = OLMKK_NUM_PARAMS;
    return PF_Err_NONE;
}

PF_Err UpdateParamsUI(PF_InData *in_data, PF_ParamDef *params[]) {
    if (!in_data->pica_basicP || !params) return PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    auto *utils = suites.ParamUtilsSuite3();
    if (!utils || !utils->PF_UpdateParamUI) return PF_Err_NONE;
    const int pairs[5][3] = {{OLMKK_V_USE_RAMP, OLMKK_V_COLOR, OLMKK_V_RAMP},
                             {OLMKK_H_USE_RAMP, OLMKK_H_COLOR, OLMKK_H_RAMP},
                             {OLMKK_D_USE_RAMP, OLMKK_D_COLOR, OLMKK_D_RAMP},
                             {OLMKK_D2_USE_RAMP, OLMKK_D2_COLOR, OLMKK_D2_RAMP},
                             {OLMKK_HL_USE_RAMP, OLMKK_HL_COLOR, OLMKK_HL_RAMP}};
    PF_Err err = PF_Err_NONE;
    for (auto &p : pairs) {
        const bool use = params[p[0]] && params[p[0]]->u.bd.value;
        for (int k = 1; k <= 2 && !err; ++k) {
            if (!params[p[k]]) continue;
            PF_ParamDef d = *params[p[k]];
            const bool disable = (k == 1) ? use : !use;
            if (disable) d.ui_flags |= PF_PUI_DISABLED;
            else d.ui_flags &= ~PF_PUI_DISABLED;
            err = utils->PF_UpdateParamUI(in_data->effect_ref, p[k], &d);
        }
    }
    return err;
}

PF_Err PreRender(PF_InData *in_data, PF_PreRenderExtra *extra) {
    PF_RenderRequest req = extra->input->output_request;
    PF_CheckoutResult result;
    AEFX_CLR_STRUCT(result);
    PF_Err err = extra->cb->checkout_layer(in_data->effect_ref, OLMKK_INPUT, 0, &req,
                                           in_data->current_time, in_data->time_step,
                                           in_data->time_scale, &result);
    if (err) return err;
    UnionLRect(&result.result_rect, &extra->output->result_rect);
    UnionLRect(&result.max_result_rect, &extra->output->max_result_rect);
    extra->output->flags = PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS;
    return PF_Err_NONE;
}

PF_Err ArbDispatch(PF_InData *in_data, PF_ArbParamsExtra *extra) {
    switch (extra->which_function) {
        case PF_Arbitrary_NEW_FUNC: {
            *extra->u.new_func_params.arbPH = NewArb(in_data, DefaultRamp());
            return *extra->u.new_func_params.arbPH ? PF_Err_NONE : PF_Err_OUT_OF_MEMORY;
        }
        case PF_Arbitrary_DISPOSE_FUNC:
            if (extra->u.dispose_func_params.arbH) PF_DISPOSE_HANDLE(extra->u.dispose_func_params.arbH);
            return PF_Err_NONE;
        case PF_Arbitrary_COPY_FUNC: {
            Ramp r = DefaultRamp();
            if (Ramp *s = LockRamp(in_data, extra->u.copy_func_params.src_arbH)) r = *s;
            *extra->u.copy_func_params.dst_arbPH = NewArb(in_data, r);
            return *extra->u.copy_func_params.dst_arbPH ? PF_Err_NONE : PF_Err_OUT_OF_MEMORY;
        }
        case PF_Arbitrary_FLAT_SIZE_FUNC:
            *extra->u.flat_size_func_params.flat_data_sizePLu = kFlatBytes;
            return PF_Err_NONE;
        case PF_Arbitrary_FLATTEN_FUNC: {
            if (extra->u.flatten_func_params.buf_sizeLu < (A_u_long)kFlatBytes) return PF_Err_OUT_OF_MEMORY;
            auto *dst = (unsigned char *)extra->u.flatten_func_params.flat_dataPV;
            dst[0] = 1;
            std::memset(dst + 1, 0, kRampBytes);
            if (Ramp *s = LockRamp(in_data, extra->u.flatten_func_params.arbH))
                std::memcpy(dst + 1, s, kRampBytes);
            return PF_Err_NONE;
        }
        case PF_Arbitrary_UNFLATTEN_FUNC: {
            Ramp r = DefaultRamp();
            const auto *src = (const unsigned char *)extra->u.unflatten_func_params.flat_dataPV;
            if (src && extra->u.unflatten_func_params.buf_sizeLu >= (A_u_long)kFlatBytes && src[0] == 1)
                std::memcpy(&r, src + 1, kRampBytes);
            *extra->u.unflatten_func_params.arbPH = NewArb(in_data, r);
            return *extra->u.unflatten_func_params.arbPH ? PF_Err_NONE : PF_Err_OUT_OF_MEMORY;
        }
        case PF_Arbitrary_INTERP_FUNC: {
            Ramp a = DefaultRamp(), b = DefaultRamp();
            if (Ramp *s = LockRamp(in_data, extra->u.interp_func_params.left_arbH)) a = *s;
            if (Ramp *s = LockRamp(in_data, extra->u.interp_func_params.right_arbH)) b = *s;
            const float t = (float)extra->u.interp_func_params.tF;
            if (a.count == b.count) {
                for (int i = 0; i < std::min(a.count, kStops); ++i) {
                    auto mix = [&](float u, float v) { return u + (v - u) * t; };
                    a.s[i].x = mix(a.s[i].x, b.s[i].x);
                    a.s[i].a = mix(a.s[i].a, b.s[i].a);
                    a.s[i].r = mix(a.s[i].r, b.s[i].r);
                    a.s[i].g = mix(a.s[i].g, b.s[i].g);
                    a.s[i].b = mix(a.s[i].b, b.s[i].b);
                }
            } else if (t >= 0.5f) {
                a = b;
            }
            *extra->u.interp_func_params.interpPH = NewArb(in_data, a);
            return *extra->u.interp_func_params.interpPH ? PF_Err_NONE : PF_Err_OUT_OF_MEMORY;
        }
        case PF_Arbitrary_COMPARE_FUNC: {
            Ramp a{}, b{};
            if (Ramp *s = LockRamp(in_data, extra->u.compare_func_params.a_arbH)) a = *s;
            if (Ramp *s = LockRamp(in_data, extra->u.compare_func_params.b_arbH)) b = *s;
            *extra->u.compare_func_params.compareP =
                std::memcmp(&a, &b, sizeof(Ramp)) ? PF_ArbCompare_NOT_EQUAL : PF_ArbCompare_EQUAL;
            return PF_Err_NONE;
        }
        case PF_Arbitrary_PRINT_SIZE_FUNC:
            *extra->u.print_size_func_params.print_sizePLu = 1;
            return PF_Err_NONE;
        case PF_Arbitrary_PRINT_FUNC:
            if (extra->u.print_func_params.print_sizeLu && extra->u.print_func_params.print_bufferPC)
                extra->u.print_func_params.print_bufferPC[0] = 0;
            return PF_Err_NONE;
        case PF_Arbitrary_SCAN_FUNC:
            return PF_Err_CANNOT_PARSE_KEYFRAME_TEXT;
        default:
            return PF_Err_UNRECOGNIZED_PARAM_TYPE;
    }
}

// --- image ops (OpenCV 4.5.5 equivalents used by the plug-in) ---------------

struct Img {
    int w = 0, h = 0, ch = 1;
    std::vector<float> p;
    float &at(int x, int y, int c) { return p[((size_t)y * w + x) * ch + c]; }
    float at(int x, int y, int c) const { return p[((size_t)y * w + x) * ch + c]; }
};

Img Make(int w, int h, int ch) {
    Img i;
    i.w = w;
    i.h = h;
    i.ch = ch;
    i.p.assign((size_t)w * h * ch, 0.f);
    return i;
}

int Reflect101(int p, int n) {
    if (n <= 1) return 0;
    for (;;) {
        if (p < 0) p = -p;
        else if (p >= n) p = 2 * n - p - 2;
        else return p;
    }
}

void Box(const Img &src, Img &dst, int kw, int kh, bool norm) {
    dst = Make(src.w, src.h, src.ch);
    if (kw < 1) kw = 1;
    if (kh < 1) kh = 1;
    const int ax = kw / 2, ay = kh / 2;
    const float inv = norm ? 1.f / (float)(kw * kh) : 1.f;
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x)
            for (int c = 0; c < src.ch; ++c) {
                float s = 0;
                for (int j = 0; j < kh; ++j)
                    for (int i = 0; i < kw; ++i) {
                        const int xx = Reflect101(x + i - ax, src.w);
                        const int yy = Reflect101(y + j - ay, src.h);
                        s += src.at(xx, yy, c);
                    }
                dst.at(x, y, c) = s * inv;
            }
}

void GaussKernel(float sigma, int ksize, std::vector<float> &k) {
    k.resize(ksize);
    const float mid = (ksize - 1) * 0.5f;
    const float den = 2.f * sigma * sigma;
    float sum = 0;
    for (int i = 0; i < ksize; ++i) {
        const float d = i - mid;
        k[i] = std::exp(-(d * d) / den);
        sum += k[i];
    }
    for (float &v : k) v /= sum;
}

void Gaussian(const Img &src, Img &dst, int kw, int kh, float sx, float sy) {
    if (sx <= 0.f) sx = 0.3f * ((kw - 1) * 0.5f - 1.f) + 0.8f;
    if (sy <= 0.f) sy = 0.3f * ((kh - 1) * 0.5f - 1.f) + 0.8f;
    std::vector<float> kx, ky;
    GaussKernel(sx, kw, kx);
    GaussKernel(sy, kh, ky);
    Img tmp = Make(src.w, src.h, src.ch);
    const int ax = kw / 2, ay = kh / 2;
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x)
            for (int c = 0; c < src.ch; ++c) {
                float s = 0;
                for (int i = 0; i < kw; ++i)
                    s += kx[i] * src.at(Reflect101(x + i - ax, src.w), y, c);
                tmp.at(x, y, c) = s;
            }
    dst = Make(src.w, src.h, src.ch);
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x)
            for (int c = 0; c < src.ch; ++c) {
                float s = 0;
                for (int j = 0; j < kh; ++j)
                    s += ky[j] * tmp.at(x, Reflect101(y + j - ay, src.h), c);
                dst.at(x, y, c) = s;
            }
}

void IIR(const Img &src, Img &dst, int k) {
    dst = Make(src.w, src.h, src.ch);
    const float a = (float)k / (float)(k + 1);
    const float b = (float)k / (float)((k + 1) * (k + 1));
    for (int y = 0; y < src.h; ++y) {
        for (int c = 0; c < src.ch; ++c) {
            dst.at(0, y, c) = 0;
            for (int x = 1; x < src.w; ++x)
                dst.at(x, y, c) = a * dst.at(x - 1, y, c) + b * src.at(x - 1, y, c);
            // Binary runs in place, so the backward pass reads the forward
            // result (second-order), not the original source.
            float acc = 0;
            for (int x = src.w - 2; x >= 0; --x) {
                acc = a * acc + b * dst.at(x + 1, y, c);
                dst.at(x, y, c) += acc;
            }
        }
        if (src.ch == 4)
            for (int x = 0; x < src.w; ++x) dst.at(x, y, 0) = src.at(x, y, 0);
    }
}

float Sample(const Img &im, float x, float y, int c) {
    if (x < -1.f || y < -1.f || x >= im.w || y >= im.h) return 0.f;
    const int x0 = (int)std::floor(x);
    const int y0 = (int)std::floor(y);
    const float fx = x - x0, fy = y - y0;
    auto pix = [&](int xx, int yy) {
        if (xx < 0 || yy < 0 || xx >= im.w || yy >= im.h) return 0.f;
        return im.at(xx, yy, c);
    };
    const float a = pix(x0, y0), b = pix(x0 + 1, y0), d = pix(x0, y0 + 1), e = pix(x0 + 1, y0 + 1);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (d * (1 - fx) + e * fx) * fy;
}

void Warp(const Img &src, Img &dst, const double m[6]) {
    dst = Make(src.w, src.h, src.ch);
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x) {
            const float sx = (float)(m[0] * x + m[1] * y + m[2]);
            const float sy = (float)(m[3] * x + m[4] * y + m[5]);
            for (int c = 0; c < src.ch; ++c) dst.at(x, y, c) = Sample(src, sx, sy, c);
        }
}

void RotMatrix(double cx, double cy, float deg, double m[6]) {
    const double rad = deg * 3.141592653589793 / 180.0;
    const double alpha = std::cos(rad), beta = std::sin(rad);
    m[0] = alpha;
    m[1] = beta;
    m[2] = (1 - alpha) * cx - beta * cy;
    m[3] = -beta;
    m[4] = alpha;
    m[5] = beta * cx + (1 - alpha) * cy;
}

Img Resize(const Img &src, int dw, int dh) {
    Img dst = Make(std::max(dw, 1), std::max(dh, 1), src.ch);
    const float sx = (float)src.w / (float)dst.w;
    const float sy = (float)src.h / (float)dst.h;
    for (int y = 0; y < dst.h; ++y)
        for (int x = 0; x < dst.w; ++x) {
            const float fx = ((float)x + 0.5f) * sx - 0.5f;
            const float fy = ((float)y + 0.5f) * sy - 0.5f;
            for (int c = 0; c < src.ch; ++c) dst.at(x, y, c) = Sample(src, fx, fy, c);
        }
    return dst;
}

void Blur(const Img &src, Img &dst, int mode, int k, bool square, bool norm) {
    if (square) {
        const int n = std::max(2 * k + 1, 1);
        if (mode == 3) Gaussian(src, dst, n, n, 0, 0);
        else         if (mode == 2 || mode == 4) {
            Img a, b;
            Box(src, a, n, n, norm);
            Box(a, b, n, n, norm);
            Box(b, dst, n, n, norm);
        } else {
            Box(src, dst, n, n, norm);
        }
        return;
    }
    const int n = std::max(k, 1);
    if (mode == 3) {
        const int kw = (4 * n + 1) | 1;
        Gaussian(src, dst, kw, 1, n * 0.5f, 1.f);
    } else if (mode == 4) {
        IIR(src, dst, n);
    } else if (mode == 2) {
        Img a, b;
        Box(src, a, n, 1, norm);
        Box(a, b, n, 1, norm);
        Box(b, dst, n, 1, norm);
    } else if (mode == 1) {
        Box(src, dst, n, 1, norm);
    } else {
        dst = Make(src.w, src.h, src.ch);
    }
}

float Transfer(float x, float gamma, float thr) {
    if (x == 0.f) return 0.f;
    if (x <= thr) return std::pow(x / thr, gamma + 1.f) * std::pow(thr, gamma);
    return std::pow(x, gamma);
}

Img MaskFrom(const Img &src, int mode, float gamma, float thr) {
    const bool color = mode == 3;
    Img m = Make(src.w, src.h, color ? 4 : 1);
    const float g = color ? gamma : std::max(gamma, 0.001f);
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x) {
            const float r = src.at(x, y, 0), gch = src.at(x, y, 1), b = src.at(x, y, 2),
                        a = src.at(x, y, 3);
            if (mode == 1) {
                m.at(x, y, 0) = std::pow(a, g);
            } else if (mode == 2) {
                m.at(x, y, 0) = Transfer(0.2126f * r + 0.7152f * gch + 0.0722f * b, g, thr) * a;
            } else if (mode == 4) {
                m.at(x, y, 0) = Transfer(std::max(r, std::max(gch, b)), g, thr) * a;
            } else {
                const float p = g + 1.f;
                m.at(x, y, 0) = std::pow(r, p) * a;
                m.at(x, y, 1) = std::pow(gch, p) * a;
                m.at(x, y, 2) = std::pow(b, p) * a;
                m.at(x, y, 3) = a;
            }
        }
    return m;
}

void PasteCenter(const Img &src, Img &dst) {
    const int x0 = dst.w / 2 - src.w / 2;
    const int y0 = dst.h / 2 - src.h / 2;
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x) {
            const int dx = x0 + x, dy = y0 + y;
            if (dx < 0 || dy < 0 || dx >= dst.w || dy >= dst.h) continue;
            for (int c = 0; c < src.ch; ++c) dst.at(dx, dy, c) = src.at(x, y, c);
        }
}

Img CropCenter(const Img &src, int w, int h) {
    Img d = Make(w, h, src.ch);
    const int x0 = src.w / 2 - w / 2;
    const int y0 = src.h / 2 - h / 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int sx = x0 + x, sy = y0 + y;
            if (sx < 0 || sy < 0 || sx >= src.w || sy >= src.h) continue;
            for (int c = 0; c < src.ch; ++c) d.at(x, y, c) = src.at(sx, sy, c);
        }
    return d;
}

Img Arm(const Img &base, float deg, int size, int mode, bool norm) {
    const float rad = deg * 3.141592653589793f / 180.f;
    const float s = std::fabs(std::sin(rad)), c = std::fabs(std::cos(rad));
    int cols = (int)(base.w * c + base.h * s + 4.f);
    int rows = (int)(base.w * s + base.h * c + 4.f);
    cols = std::max(cols, base.w + 4);
    rows = std::max(rows, base.h + 4);
    Img canvas = Make(cols, rows, base.ch);
    PasteCenter(base, canvas);
    double m[6];
    RotMatrix(cols * 0.5, rows * 0.5, deg, m);
    Img spun;
    Warp(canvas, spun, m);
    Img blurred;
    Blur(spun, blurred, mode, size, false, norm);
    RotMatrix(cols * 0.5, rows * 0.5, -deg, m);
    Img back;
    Warp(blurred, back, m);
    return CropCenter(back, base.w, base.h);
}

struct Spark {
    float gain, gamma, thresh, res_amt, src_amt;
    int channel, merge, blur;
    int sizes[5];
    float color[5][4];
    bool use_ramp[5];
    Ramp ramp[5];
    float angle_deg;
    bool half;
};

void Compose(const Img dirs[5], const Spark &sp, const float vol[5], Img &dst) {
    dst = Make(dirs[0].w, dirs[0].h, 4);
    const bool color = sp.channel == 3;
    for (int y = 0; y < dst.h; ++y)
        for (int x = 0; x < dst.w; ++x) {
            float acc[4] = {};
            for (int d = 0; d < 5; ++d) {
                if (color) {
                    const float a = dirs[d].at(x, y, 3);
                    if (a <= 0.001f || vol[d] == 0.f) continue;
                    const float t = Clamp01(a / vol[d]);
                    acc[1] += (dirs[d].at(x, y, 0) / a) * t * sp.gain;
                    acc[2] += (dirs[d].at(x, y, 1) / a) * t * sp.gain;
                    acc[3] += (dirs[d].at(x, y, 2) / a) * t * sp.gain;
                    acc[0] += t;
                } else {
                    const float v = dirs[d].at(x, y, 0);
                    if (v <= 0.001f) continue;
                    const float t = Clamp01(v * sp.gain);
                    float col[4];
                    if (sp.use_ramp[d]) EvalRamp(sp.ramp[d], t, col);
                    else {
                        col[0] = sp.color[d][0];
                        col[1] = sp.color[d][1];
                        col[2] = sp.color[d][2];
                        col[3] = sp.color[d][3];
                    }
                    acc[1] += col[1] * t;
                    acc[2] += col[2] * t;
                    acc[3] += col[3] * t;
                    acc[0] = acc[0] + t - acc[0] * t;
                }
            }
            if (acc[0] > 0.f) {
                acc[0] = Clamp01(acc[0]);
                acc[1] /= acc[0];
                acc[2] /= acc[0];
                acc[3] /= acc[0];
            }
            dst.at(x, y, 0) = acc[1];
            dst.at(x, y, 1) = acc[2];
            dst.at(x, y, 2) = acc[3];
            dst.at(x, y, 3) = acc[0];
        }
}

Img Sparkle(const Img &src, const Spark &sp) {
    const bool color = sp.channel == 3;
    const bool norm = !color;
    Img mask = MaskFrom(src, sp.channel, sp.gamma, sp.thresh);
    Img dirs[5];
    float vol[5] = {};
    const float angs[4] = {sp.angle_deg + 90.f, sp.angle_deg, sp.angle_deg + 45.f,
                           sp.angle_deg + 135.f};
    for (int d = 0; d < 4; ++d) {
        if (sp.sizes[d] <= 0) {
            dirs[d] = Make(src.w, src.h, mask.ch);
            continue;
        }
        dirs[d] = Arm(mask, angs[d], sp.sizes[d], sp.blur, norm);
        const int k = sp.sizes[d];
        vol[d] = sp.blur == 2 ? (float)k * k * k : (float)k;
    }
    if (sp.sizes[4] <= 0) {
        dirs[4] = Make(src.w, src.h, mask.ch);
    } else {
        Blur(mask, dirs[4], sp.blur, sp.sizes[4], true, norm);
        const int n = 2 * sp.sizes[4] + 1;
        vol[4] = (sp.blur == 2 || sp.blur == 4) ? (float)n * n * n * n : (float)n * n;
    }
    Img out;
    Compose(dirs, sp, vol, out);
    return out;
}

template <typename Pix>
Img Decode(const PF_EffectWorld *w) {
    Img im = Make(w->width, w->height, 4);
    const float sc = sizeof(Pix) == sizeof(PF_Pixel) ? 1.f / 255.f
                     : sizeof(Pix) == sizeof(PF_Pixel16) ? 1.f / 32768.f
                                                         : 1.f;
    for (int y = 0; y < w->height; ++y) {
        const Pix *row = (const Pix *)((const char *)w->data + (size_t)y * w->rowbytes);
        for (int x = 0; x < w->width; ++x) {
            im.at(x, y, 0) = row[x].red * sc;
            im.at(x, y, 1) = row[x].green * sc;
            im.at(x, y, 2) = row[x].blue * sc;
            im.at(x, y, 3) = row[x].alpha * sc;
        }
    }
    return im;
}

template <>
Img Decode<PF_PixelFloat>(const PF_EffectWorld *w) {
    Img im = Make(w->width, w->height, 4);
    for (int y = 0; y < w->height; ++y) {
        const PF_PixelFloat *row =
            (const PF_PixelFloat *)((const char *)w->data + (size_t)y * w->rowbytes);
        for (int x = 0; x < w->width; ++x) {
            im.at(x, y, 0) = row[x].red;
            im.at(x, y, 1) = row[x].green;
            im.at(x, y, 2) = row[x].blue;
            im.at(x, y, 3) = row[x].alpha;
        }
    }
    return im;
}

template <typename Pix>
void WritePix(Pix &p, float r, float g, float b, float a, float scale) {
    p.alpha = (decltype(p.alpha))(a * scale);
    p.red = (decltype(p.red))(r * scale);
    p.green = (decltype(p.green))(g * scale);
    p.blue = (decltype(p.blue))(b * scale);
}

template <>
void WritePix<PF_PixelFloat>(PF_PixelFloat &p, float r, float g, float b, float a, float) {
    p.alpha = a;
    p.red = r;
    p.green = g;
    p.blue = b;
}

template <typename Pix>
void Composite(const Img &result, const Img &src, PF_EffectWorld *dst, const Spark &sp, float scale) {
    // Binary only writes dst for merge 1 (premul) or 2 (add); any other value
    // leaves the output world untouched.
    if (sp.merge != 1 && sp.merge != 2) return;
    for (int y = 0; y < dst->height && y < result.h; ++y) {
        Pix *row = (Pix *)((char *)dst->data + (size_t)y * dst->rowbytes);
        for (int x = 0; x < dst->width && x < result.w; ++x) {
            const float ra = result.at(x, y, 3), sa = src.at(x, y, 3);
            if (ra + sa == 0.f) {
                WritePix(row[x], 0, 0, 0, 0, scale);
                continue;
            }
            const float rA = Clamp01(ra * sp.res_amt), sA = Clamp01(sa * sp.src_amt);
            float o[3] = {}, oa = 0;
            if (sp.merge == 1) {
                const float s = sA + rA;
                if (s > 0.f) {
                    for (int c = 0; c < 3; ++c)
                        o[c] = Clamp01((sA * src.at(x, y, c) + rA * result.at(x, y, c)) / s);
                    oa = Clamp01(s);
                }
            } else {
                for (int c = 0; c < 3; ++c)
                    o[c] = Clamp01(sA * src.at(x, y, c) + rA * result.at(x, y, c));
                oa = Clamp01(sA + rA);
            }
            WritePix(row[x], o[0], o[1], o[2], oa, scale);
        }
    }
}

float Col(const PF_Pixel &p, int i) {
    const A_u_char v[4] = {p.alpha, p.red, p.green, p.blue};
    return v[i] / 255.f;
}

PF_Err Checkout(PF_InData *in, Spark &sp) {
    auto get = [&](int pos, PF_ParamDef &d) {
        AEFX_CLR_STRUCT(d);
        return PF_CHECKOUT_PARAM(in, pos, in->current_time, in->time_step, in->time_scale, &d);
    };
    PF_ParamDef d;
    PF_Err err = get(OLMKK_ROTATION, d);
    if (err) return err;
    sp.angle_deg = (float)d.u.ad.value / 65536.f;
    PF_CHECKIN_PARAM(in, &d);
    err = get(OLMKK_GAIN, d);
    if (err) return err;
    sp.gain = (float)d.u.fs_d.value;
    PF_CHECKIN_PARAM(in, &d);
    err = get(OLMKK_STRENGTH, d);
    if (err) return err;
    sp.gamma = d.u.sd.value * 0.01f;
    PF_CHECKIN_PARAM(in, &d);
    err = get(OLMKK_FADE, d);
    if (err) return err;
    sp.thresh = (float)d.u.fs_d.value * 0.2f;
    PF_CHECKIN_PARAM(in, &d);
    const int sliders[5] = {OLMKK_V_LEN, OLMKK_H_LEN, OLMKK_D_LEN, OLMKK_D2_LEN, OLMKK_HL_RADIUS};
    for (int i = 0; i < 5; ++i) {
        err = get(sliders[i], d);
        if (err) return err;
        sp.sizes[i] = d.u.sd.value;
        PF_CHECKIN_PARAM(in, &d);
    }
    err = get(OLMKK_GLOW_OPACITY, d);
    if (err) return err;
    sp.res_amt = d.u.sd.value * 0.01f;
    PF_CHECKIN_PARAM(in, &d);
    err = get(OLMKK_SRC_OPACITY, d);
    if (err) return err;
    sp.src_amt = d.u.sd.value * 0.01f;
    PF_CHECKIN_PARAM(in, &d);
    err = get(OLMKK_CHANNEL, d);
    if (err) return err;
    sp.channel = d.u.pd.value;
    PF_CHECKIN_PARAM(in, &d);
    err = get(OLMKK_MERGE, d);
    if (err) return err;
    sp.merge = d.u.pd.value;
    PF_CHECKIN_PARAM(in, &d);
    err = get(OLMKK_BLUR_MODE, d);
    if (err) return err;
    sp.blur = d.u.pd.value;
    PF_CHECKIN_PARAM(in, &d);
    err = get(OLMKK_APPROX, d);
    if (err) return err;
    sp.half = d.u.bd.value != 0;
    PF_CHECKIN_PARAM(in, &d);
    const int colors[5] = {OLMKK_V_COLOR, OLMKK_H_COLOR, OLMKK_D_COLOR, OLMKK_D2_COLOR, OLMKK_HL_COLOR};
    for (int i = 0; i < 5; ++i) {
        err = get(colors[i], d);
        if (err) return err;
        for (int c = 0; c < 4; ++c) sp.color[i][c] = Col(d.u.cd.value, c);
        PF_CHECKIN_PARAM(in, &d);
    }
    const int uses[5] = {OLMKK_V_USE_RAMP, OLMKK_H_USE_RAMP, OLMKK_D_USE_RAMP, OLMKK_D2_USE_RAMP,
                         OLMKK_HL_USE_RAMP};
    const int ramps[5] = {OLMKK_V_RAMP, OLMKK_H_RAMP, OLMKK_D_RAMP, OLMKK_D2_RAMP, OLMKK_HL_RAMP};
    for (int i = 0; i < 5; ++i) {
        err = get(uses[i], d);
        if (err) return err;
        sp.use_ramp[i] = d.u.bd.value != 0;
        PF_CHECKIN_PARAM(in, &d);
        err = get(ramps[i], d);
        if (err) return err;
        sp.ramp[i] = DefaultRamp();
        if (Ramp *r = LockRamp(in, d.u.arb_d.value)) sp.ramp[i] = *r;
        PF_CHECKIN_PARAM(in, &d);
    }
    return PF_Err_NONE;
}

template <typename Pix>
PF_Err RenderDepth(PF_InData *in, PF_EffectWorld *input, PF_EffectWorld *output, Spark sp,
                   float scale) {
    const float ds = (float)in->downsample_x.num / (float)in->downsample_x.den;
    const bool half = sp.half && ds > 0.5f;
    float sc = ds;
    int ww = input->width, wh = input->height;
    if (half) {
        sc *= 0.5f;
        ww = (int)(input->width * 0.5f);
        wh = (int)(input->height * 0.5f);
    }
    for (int i = 0; i < 5; ++i) sp.sizes[i] = (int)(sp.sizes[i] * sc);
    if (sp.sizes[0] + sp.sizes[1] + sp.sizes[2] + sp.sizes[3] + sp.sizes[4] == 0)
        return in->utils->copy(in->effect_ref, input, output, nullptr, nullptr);
    Img full = Decode<Pix>(input);
    Img work = half ? Resize(full, ww, wh) : full;
    Img spark = Sparkle(work, sp);
    if (half) spark = Resize(spark, full.w, full.h);
    Composite<Pix>(spark, full, output, sp, scale);
    return PF_Err_NONE;
}

PF_Err SmartRender(PF_InData *in_data, PF_SmartRenderExtra *extra) {
    const int depth = extra->input->bitdepth;
    if (depth != 8 && depth != 16 && depth != 32) return PF_Err_NONE;
    PF_EffectWorld *input = nullptr;
    PF_EffectWorld *output = nullptr;
    PF_Err err = extra->cb->checkout_layer_pixels(in_data->effect_ref, OLMKK_INPUT, &input);
    if (!err) err = extra->cb->checkout_output(in_data->effect_ref, &output);
    if (!err && (!input || !output)) err = PF_Err_BAD_CALLBACK_PARAM;
    Spark sp{};
    if (!err) err = Checkout(in_data, sp);
    if (err) return err;
    if (depth == 8) return RenderDepth<PF_Pixel>(in_data, input, output, sp, 255.f);
    if (depth == 16) return RenderDepth<PF_Pixel16>(in_data, input, output, sp, 32768.f);
    return RenderDepth<PF_PixelFloat>(in_data, input, output, sp, 1.f);
}

}  // namespace

PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[],
                  PF_LayerDef *output, void *extra) {
    (void)output;
    switch (cmd) {
        case PF_Cmd_ABOUT: return About(in_data, out_data);
        case PF_Cmd_GLOBAL_SETUP: return GlobalSetup(in_data, out_data);
        case PF_Cmd_PARAMS_SETUP: return ParamsSetup(in_data, out_data);
        case PF_Cmd_UPDATE_PARAMS_UI: return UpdateParamsUI(in_data, params);
        case PF_Cmd_SMART_PRE_RENDER: return PreRender(in_data, (PF_PreRenderExtra *)extra);
        case PF_Cmd_SMART_RENDER: return SmartRender(in_data, (PF_SmartRenderExtra *)extra);
        case PF_Cmd_ARBITRARY_CALLBACK: return ArbDispatch(in_data, (PF_ArbParamsExtra *)extra);
        default: return PF_Err_NONE;
    }
}

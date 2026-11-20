/* OLM UV Map 1.1.1: macOS port of OLMUVMap.aex (binary dispatch at 0x180005210).
 * Smart-render only. The UV layer's R (u) and 1-G (v) address the Texture layer.
 * Interpolation: Nearest / Bilinear / Trilinear (float mip chain, LOD from the
 * UV footprint, Smoothness lerps min..max footprint).
 * Spec with IDA addresses: %TEMP%\opencode\specs\OLMUVMap.md */

#include "OLMUVMap.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

static_assert(PF_VERSION(OLMUV_MAJOR_VERSION, OLMUV_MINOR_VERSION, OLMUV_BUG_VERSION,
                         PF_Stage_DEVELOP, 0) == 559104,
              "UV Map version must match its PiPL");

// Binary: out_flags 0x02000040, out_flags2 0x18001400. The port adds
// SEND_UPDATE_PARAMS_UI so the Smoothness-disable rule (spec section 4) runs; see spec section 9.
constexpr PF_OutFlags kOutFlags =
    PF_OutFlag_USE_OUTPUT_EXTENT | PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_SEND_UPDATE_PARAMS_UI;
constexpr PF_OutFlags2 kOutFlags2 =
    PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |
    PF_OutFlag2_SUPPORTS_THREADED_RENDERING | PF_OutFlag2_MUTABLE_RENDER_SEQUENCE_DATA_SLOWER;
static_assert(kOutFlags == 0x06000040, "out_flags");
static_assert(kOutFlags2 == 0x18001400, "out_flags2");

// Binary keeps a 48-byte per-instance block; word 10 (byte 40) is the max mip level, -1 until set.
struct SeqBlock {
    std::int32_t words[12];
};
static_assert(sizeof(SeqBlock) == 48, "sequence block size");

// Pixel storage type per depth. Four channels per pixel, order A,R,G,B.
template <int BPC>
struct PxType {
    using T = float;
};
template <>
struct PxType<8> {
    using T = std::uint8_t;
};
template <>
struct PxType<16> {
    using T = std::uint16_t;
};

// ---- image views -----------------------------------------------------------

struct View {
    const char *data = nullptr;
    long rowbytes = 0;
    int w = 0;
    int h = 0;
};

View ViewOf(const PF_EffectWorld *world) {
    View v;
    v.data = world ? reinterpret_cast<const char *>(world->data) : nullptr;
    v.rowbytes = world ? world->rowbytes : 0;
    v.w = world ? world->width : 0;
    v.h = world ? world->height : 0;
    return v;
}

// Binary clamp (sub_180001A40 and friends): clamp into [0, n-1].
inline int ClampIdx(int v, int n) {
    if (v >= n - 1) v = n - 1;
    return v > 0 ? v : 0;
}

// Pixel fetch with edge clamp. Returns a zero pixel for an empty or missing image.
template <class T>
const T *Fetch(const View &v, int x, int y) {
    static const T kZero[4] = {};
    if (!v.data || v.w <= 0 || v.h <= 0) return kZero;
    x = ClampIdx(x, v.w);
    y = ClampIdx(y, v.h);
    return reinterpret_cast<const T *>(v.data + static_cast<long>(v.rowbytes) * y) + 4 * x;
}

// ---- depth helpers ---------------------------------------------------------

// Binary normalisation: 8 bpc /255 (double divide), 16 bpc * 0.000030517578f, 32 bpc raw.
template <int BPC>
float NormChan(float raw) {
    if (BPC == 8) return static_cast<float>(raw / 255.0);
    if (BPC == 16) return raw * 0.000030517578f;
    return raw;
}

// Binary clamp used for UV and alpha factors: fmax(v,0), then 1 if >= 1.
inline float Clamp01(float v) {
    v = std::fmax(v, 0.0f);
    if (v >= 1.0f) v = 1.0f;
    return v;
}

// Truncating store to the integer depths (binary (int) then implicit narrowing).
template <class T>
T StoreInt(float v) {
    return static_cast<T>(static_cast<int>(v));
}

// (int)(float)((double)v * scale): the binary multiplies a float by a double scale.
template <class T>
T StoreScaled(float v, double scale) {
    return static_cast<T>(static_cast<int>(static_cast<float>(static_cast<double>(v) * scale)));
}

// ---- float mip levels and bilinear sampling --------------------------------

struct MipLevel {
    int w = 0;
    int h = 0;
    std::vector<float> px;  // 4 floats per pixel (A,R,G,B), row-major, rowbytes = w * 16
};

View MipView(const MipLevel &m) {
    View v;
    v.data = reinterpret_cast<const char *>(m.px.data());
    v.rowbytes = static_cast<long>(m.w) * 16;
    v.w = m.w;
    v.h = m.h;
    return v;
}

// Bilinear with clamped taps. Same expression tree as binary sub_180001770 (float levels)
// and sub_1800014A0 / sub_1800011C0 (integer textures, raw values). Output A,R,G,B floats.
template <class T>
void Bilerp(const View &v, int x, int y, float fx, float fy, float out[4]) {
    const T *p00 = Fetch<T>(v, x, y);
    const T *p10 = Fetch<T>(v, x + 1, y);
    const T *p01 = Fetch<T>(v, x, y + 1);
    const T *p11 = Fetch<T>(v, x + 1, y + 1);
    const float omfx = static_cast<float>(1.0 - fx);
    const float omfy = static_cast<float>(1.0 - fy);
    for (int c = 0; c < 4; ++c) {
        const float top = static_cast<float>(
            static_cast<float>(static_cast<float>(p00[c]) * omfx) +
            static_cast<float>(static_cast<float>(p10[c]) * fx));
        const float bot = static_cast<float>(
            static_cast<float>(static_cast<float>(p01[c]) * omfx) +
            static_cast<float>(static_cast<float>(p11[c]) * fx));
        out[c] = static_cast<float>(static_cast<float>(top * omfy) + static_cast<float>(bot * fy));
    }
}

// ---- render context --------------------------------------------------------

struct Ctx {
    View input;    // UV layer
    View texture;  // Texture layer (copy of the checked-out PF_LayerDef)
    int mode = OLMUV_NEAREST;
    float smooth = 0.0f;
    std::vector<MipLevel> mips;  // Trilinear only; mips[0] is texture-sized
};

// ---- trilinear neighbour statistics (binary sub_180004780) -----------------

struct Footprint {
    float maxU, minU, maxV, minV;
};

// nR/nG: clamped normalised R/G; nA: raw alpha. Index 0 = centre, 1..4 = left, right, up, down.
Footprint FootprintOf(const float nR[5], const float nG[5], const float nA[5]) {
    float dU[4], dV[4];
    bool valid[4];
    for (int k = 0; k < 4; ++k) {
        dU[k] = std::fabs(nR[k + 1] - nR[0]);
        dV[k] = std::fabs(nG[k + 1] - nG[0]);
        valid[k] = nA[k + 1] > 0.0f;
    }
    Footprint f{0.0f, 0.0f, 0.0f, 0.0f};
    for (int k = 0; k < 4; ++k) {
        if (!valid[k]) continue;
        if (dU[k] > f.maxU) f.maxU = dU[k];
        if (dV[k] > f.maxV) f.maxV = dV[k];
    }
    f.minU = f.maxU;
    f.minV = f.maxV;
    for (int k = 0; k < 4; ++k) {
        if (!valid[k]) continue;
        if (dU[k] > 0.0f && f.minU > dU[k]) f.minU = dU[k];
        if (dV[k] > 0.0f && f.minV > dV[k]) f.minV = dV[k];
    }
    return f;
}

// ---- per-pixel kernel ------------------------------------------------------

template <int BPC>
void RenderPixel(const Ctx &c, int x, int y, const typename PxType<BPC>::T *in,
                 typename PxType<BPC>::T *out) {
    using T = typename PxType<BPC>::T;
    // Binary alpha guard: 8/16 bpc raw != 0, 32 bpc > 0.0.
    const bool alphaOk = (BPC == 32) ? (static_cast<float>(in[0]) > 0.0f) : (in[0] != 0);
    const View &tex = c.texture;
    if (!alphaOk || !tex.data || tex.w <= 0 || tex.h <= 0) {
        for (int i = 0; i < 4; ++i) out[i] = T(0);
        return;
    }
    const float R = Clamp01(NormChan<BPC>(static_cast<float>(in[1])));
    const float G = Clamp01(NormChan<BPC>(static_cast<float>(in[2])));
    const float inA = Clamp01(NormChan<BPC>(static_cast<float>(in[0])));
    const float Wm1 = static_cast<float>(tex.w - 1);
    const float Hm1 = static_cast<float>(tex.h - 1);
    const float V = static_cast<float>(1.0 - G);  // binary v = 1 - G (vertical flip)

    if (c.mode == OLMUV_NEAREST) {
        const float X = std::fmin(std::fmax(Wm1 * R, 0.0f), Wm1);
        const float Y = std::fmin(std::fmax(Hm1 * V, 0.0f), Hm1);
        const T *t = Fetch<T>(tex, static_cast<int>(X), static_cast<int>(Y));
        for (int i = 1; i < 4; ++i) out[i] = t[i];
        const float a = static_cast<float>(inA * static_cast<float>(t[0]));
        out[0] = (BPC == 32) ? static_cast<T>(a) : StoreInt<T>(a);
        return;
    }

    if (c.mode == OLMUV_BILINEAR) {
        const float X = std::fmin(std::fmax(Wm1 * R, 0.0f), Wm1);
        const float Y = std::fmin(std::fmax(Hm1 * V, 0.0f), Hm1);
        const float x0 = std::floor(X);
        const float y0 = std::floor(Y);
        float res[4];
        Bilerp<T>(tex, static_cast<int>(x0), static_cast<int>(y0), X - x0, Y - y0, res);
        if (BPC == 32) {
            for (int i = 1; i < 4; ++i) out[i] = res[i];
            out[0] = static_cast<float>(inA * res[0]);
        } else {
            for (int i = 1; i < 4; ++i) out[i] = StoreInt<T>(res[i]);
            // Binary: alpha is truncated to the integer depth before the factor multiply.
            const float aInt = static_cast<float>(StoreInt<T>(res[0]));
            out[0] = StoreInt<T>(static_cast<float>(inA * aInt));
        }
        return;
    }

    // Trilinear (also the binary's fall-through for popup values outside 1..2).
    static const int kDx[4] = {-1, 1, 0, 0};
    static const int kDy[4] = {0, 0, -1, 1};
    float nR[5], nG[5], nA[5];
    nR[0] = R;
    nG[0] = G;
    nA[0] = static_cast<float>(in[0]);
    for (int k = 0; k < 4; ++k) {
        const T *p = Fetch<T>(c.input, x + kDx[k], y + kDy[k]);
        nR[k + 1] = Clamp01(NormChan<BPC>(static_cast<float>(p[1])));
        nG[k + 1] = Clamp01(NormChan<BPC>(static_cast<float>(p[2])));
        nA[k + 1] = static_cast<float>(p[0]);
    }
    const Footprint f = FootprintOf(nR, nG, nA);
    // Footprint in texels: U scales by (W-1), V by (H-1). lo/hi are the min and max of the three.
    const float v57 = std::fmin(Hm1 * f.minV, Hm1 * f.maxV);
    const float v59 = Wm1 * f.maxU;
    const float v60 = Wm1 * f.minU;
    const float lo = std::fmin(std::fmin(v57, v59), v60);
    const float hi = std::fmax(std::fmax(v57, v59), v60);
    float footprint = static_cast<float>(static_cast<float>(1.0 - c.smooth) * lo) +
                      static_cast<float>(c.smooth * hi);
    if (footprint <= 0.0f) footprint = 0.0001f;
    const float lod = std::log(footprint) / std::log(2.0f);

    const float maxL = static_cast<float>(c.mips.size() - 1);
    const float flo = std::floor(lod);
    const float flo0 = flo >= 0.0f ? flo : 0.0f;
    const int lvl0 = static_cast<int>(maxL <= flo0 ? maxL : flo0);
    float flo1 = std::fmax(0.0f, flo + 1.0f);
    if (maxL <= flo1) flo1 = maxL;
    const int lvl1 = static_cast<int>(flo1);
    float tl = std::fmax(0.0f, lod);
    if (maxL <= tl) tl = maxL;
    const float t = tl - std::floor(tl);

    float L[2][4];
    const int lv[2] = {lvl0, lvl1};
    for (int k = 0; k < 2; ++k) {
        const MipLevel &m = c.mips[static_cast<std::size_t>(lv[k])];
        const float fw = static_cast<float>(m.w - 1);
        const float fh = static_cast<float>(m.h - 1);
        const float xs = std::fmin(std::fmax(R * fw, 0.0f), fw);
        const float ys = std::fmin(std::fmax(V * fh, 0.0f), fh);
        Bilerp<float>(MipView(m), static_cast<int>(xs), static_cast<int>(ys), xs - std::floor(xs),
                      ys - std::floor(ys), L[k]);
    }
    const float omt = static_cast<float>(1.0 - t);
    const float bA = static_cast<float>(L[0][0] * omt) + static_cast<float>(L[1][0] * t);
    const float bR = static_cast<float>(L[0][1] * omt) + static_cast<float>(L[1][1] * t);
    const float bG = static_cast<float>(L[0][2] * omt) + static_cast<float>(L[1][2] * t);
    const float bB = static_cast<float>(L[0][3] * omt) + static_cast<float>(L[1][3] * t);
    const float aOut = static_cast<float>(bA * inA);
    if (BPC == 32) {
        out[0] = aOut;
        out[1] = bR;
        out[2] = bG;
        out[3] = bB;
    } else {
        const double scale = (BPC == 8) ? 255.0 : 32768.0;
        out[0] = StoreScaled<T>(aOut, scale);
        out[1] = StoreScaled<T>(bR, scale);
        out[2] = StoreScaled<T>(bG, scale);
        out[3] = StoreScaled<T>(bB, scale);
    }
}

// ---- mip chain -------------------------------------------------------------

// Level 0 = texture, normalised per depth without clamp (binary sub_180004AB0).
template <int BPC>
void FillLevel0(const View &tex, MipLevel &m) {
    using T = typename PxType<BPC>::T;
    for (int y = 0; y < m.h; ++y) {
        const T *row = reinterpret_cast<const T *>(tex.data + static_cast<long>(tex.rowbytes) * y);
        for (int x = 0; x < m.w; ++x) {
            const T *p = row + 4 * x;
            float *d = &m.px[(static_cast<std::size_t>(y) * m.w + x) * 4];
            for (int c = 0; c < 4; ++c) d[c] = NormChan<BPC>(static_cast<float>(p[c]));
        }
    }
}

// Level L from level L-1 (binary sub_180004AB0 second half). Even source size: 2x2 box.
// Odd source size: 3 taps with weights {n-i, n, i+1} / (2n+1), n = destination size.
void Downsample(const MipLevel &src, MipLevel &dst) {
    const int pw = src.w, ph = src.h, cw = dst.w, ch = dst.h;
    for (int r = 0; r < ch; ++r) {
        float wy[3];
        if (ph & 1) {
            const float d = static_cast<float>(2 * ch + 1);
            wy[0] = static_cast<float>(ch - r) / d;
            wy[1] = static_cast<float>(ch) / d;
            wy[2] = static_cast<float>(r + 1) / d;
        } else {
            wy[0] = 0.5f;
            wy[1] = 0.5f;
            wy[2] = 0.0f;
        }
        const int sy[3] = {2 * r, std::min(2 * r + 1, ph - 1), std::min(2 * r + 2, ph - 1)};
        for (int c = 0; c < cw; ++c) {
            float wx[3];
            if (pw & 1) {
                const float d = static_cast<float>(2 * cw + 1);
                wx[0] = static_cast<float>(cw - c) / d;
                wx[1] = static_cast<float>(cw) / d;
                wx[2] = static_cast<float>(c + 1) / d;
            } else {
                wx[0] = 0.5f;
                wx[1] = 0.5f;
                wx[2] = 0.0f;
            }
            const int sx[3] = {2 * c, std::min(2 * c + 1, pw - 1), std::min(2 * c + 2, pw - 1)};
            float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int k = 0; k < 3; ++k) {
                const float w0 = wy[k] * wx[0];
                const float w1 = wy[k] * wx[1];
                const float w2 = wy[k] * wx[2];
                const float *p0 = &src.px[(static_cast<std::size_t>(sy[k]) * pw + sx[0]) * 4];
                const float *p1 = &src.px[(static_cast<std::size_t>(sy[k]) * pw + sx[1]) * 4];
                const float *p2 = &src.px[(static_cast<std::size_t>(sy[k]) * pw + sx[2]) * 4];
                for (int ch4 = 0; ch4 < 4; ++ch4) {
                    float a = static_cast<float>(static_cast<float>(w0 * p0[ch4]) + acc[ch4]);
                    a = static_cast<float>(a + static_cast<float>(w1 * p1[ch4]));
                    acc[ch4] = static_cast<float>(a + static_cast<float>(w2 * p2[ch4]));
                }
            }
            float *d = &dst.px[(static_cast<std::size_t>(r) * cw + c) * 4];
            for (int ch4 = 0; ch4 < 4; ++ch4) d[ch4] = acc[ch4];
        }
    }
}

// Mip count: maxLevel = floor(log2(max(W,H))) with the binary's float/double split.
template <int BPC>
std::vector<MipLevel> BuildMips(const View &tex) {
    const int W = tex.w, H = tex.h;
    const int mx = std::max(W, H);
    const float v14 = static_cast<float>(static_cast<double>(std::log(static_cast<float>(mx))) /
                                         std::log(2.0));
    const int maxLevel = static_cast<int>(std::floor(v14));
    std::vector<MipLevel> mips(static_cast<std::size_t>(maxLevel) + 1);
    for (int i = 0; i <= maxLevel; ++i) {
        MipLevel &m = mips[static_cast<std::size_t>(i)];
        m.w = std::max(W >> i, 1);
        m.h = std::max(H >> i, 1);
        m.px.assign(static_cast<std::size_t>(m.w) * m.h * 4, 0.0f);
    }
    FillLevel0<BPC>(tex, mips[0]);
    for (int i = 1; i <= maxLevel; ++i) {
        Downsample(mips[static_cast<std::size_t>(i) - 1], mips[static_cast<std::size_t>(i)]);
    }
    return mips;
}

// ---- command handlers ------------------------------------------------------

PF_Err About(PF_InData *in_data, PF_OutData *out_data) {
    PF_SPRINTF(out_data->return_msg, "%s v%d.%d.%d\r%s", OLMUV_NAME, OLMUV_MAJOR_VERSION,
               OLMUV_MINOR_VERSION, OLMUV_BUG_VERSION, OLMUV_DESC);
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_OutData *out_data) {
    out_data->my_version = PF_VERSION(OLMUV_MAJOR_VERSION, OLMUV_MINOR_VERSION, OLMUV_BUG_VERSION,
                                      PF_Stage_DEVELOP, 0);
    out_data->out_flags = kOutFlags;
    out_data->out_flags2 = kOutFlags2;
    return PF_Err_NONE;
}

PF_Err ParamsSetup(PF_InData *in_data, PF_OutData *out_data) {
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Texture", PF_LayerDefault_MYSELF, OLMUV_TEXTURE);  // binary: v22[43] = -1
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Interpolation Method", 3, OLMUV_TRILINEAR, "Nearest Neighbor|Bilinear|Trilinear",
                 OLMUV_INTERP);
    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_SUPERVISE;
    PF_ADD_FLOAT_SLIDER("Smoothness", 0, 1, 0, 1, 3.0, 0.5, 2, PF_ValueDisplayFlag_NONE, false,
                        OLMUV_SMOOTHNESS);
    out_data->num_params = OLMUV_NUM_PARAMS;
    return PF_Err_NONE;
}

// Binary sub_180003B60 / inline case 6: 48-byte handle, zeroed, max level -1.
PF_Err SequenceSetup(PF_InData *in_data, PF_OutData *out_data) {
    if (out_data->sequence_data) in_data->utils->host_dispose_handle(out_data->sequence_data);
    out_data->sequence_data = in_data->utils->host_new_handle(sizeof(SeqBlock));
    if (!out_data->sequence_data) return PF_Err_OUT_OF_MEMORY;
    auto *s = static_cast<SeqBlock *>(in_data->utils->host_lock_handle(out_data->sequence_data));
    if (!s) return PF_Err_OUT_OF_MEMORY;
    std::memset(s, 0, sizeof(SeqBlock));
    s->words[10] = -1;
    in_data->utils->host_unlock_handle(out_data->sequence_data);
    return PF_Err_NONE;
}

PF_Err SequenceSetDown(PF_InData *in_data, PF_OutData *out_data) {
    if (in_data->sequence_data) {
        in_data->utils->host_dispose_handle(in_data->sequence_data);
        out_data->sequence_data = nullptr;
    }
    return PF_Err_NONE;
}

// Binary sub_180004080: a popup other than Trilinear greys out Smoothness (ui_flags = PF_PUI_DISABLED).
PF_Err UpdateParamsUI(PF_InData *in_data, PF_OutData *out_data) {
    PF_ParamDef popup;
    AEFX_CLR_STRUCT(popup);
    PF_Err err = PF_CHECKOUT_PARAM(in_data, OLMUV_INTERP, in_data->current_time, in_data->time_step,
                                   in_data->time_scale, &popup);
    if (err) return err;
    const A_long mode = popup.u.pd.value;
    PF_CHECKIN_PARAM(in_data, &popup);

    PF_ParamDef smooth;
    AEFX_CLR_STRUCT(smooth);
    err = PF_CHECKOUT_PARAM(in_data, OLMUV_SMOOTHNESS, in_data->current_time, in_data->time_step,
                            in_data->time_scale, &smooth);
    if (err) return err;
    smooth.ui_flags = (mode != OLMUV_TRILINEAR) ? PF_PUI_DISABLED : 0;
    {
        AEFX_SuiteScoper<PF_ParamUtilsSuite3> suite(in_data, kPFParamUtilsSuite,
                                                    kPFParamUtilsSuiteVersion3, out_data);
        err = suite->PF_UpdateParamUI(in_data->effect_ref, OLMUV_SMOOTHNESS, &smooth);
    }
    const PF_Err cin = PF_CHECKIN_PARAM(in_data, &smooth);
    return err ? err : cin;
}

PF_Err PreRender(PF_InData *in_data, PF_PreRenderExtra *extra) {
    PF_RenderRequest req = extra->input->output_request;
    PF_CheckoutResult result;
    AEFX_CLR_STRUCT(result);
    const PF_Err err = extra->cb->checkout_layer(in_data->effect_ref, OLMUV_INPUT, 0, &req,
                                                 in_data->current_time, in_data->time_step,
                                                 in_data->time_scale, &result);
    if (err) return err;
    UnionLRect(&result.result_rect, &extra->output->result_rect);
    UnionLRect(&result.max_result_rect, &extra->output->max_result_rect);
    return PF_Err_NONE;
}

// Iterate the output extent with the per-pixel kernel (binary PF_Iterate*, serial here).
template <int BPC>
void RenderAll(const Ctx &ctx, const PF_EffectWorld *input, PF_EffectWorld *output) {
    using T = typename PxType<BPC>::T;
    const PF_Rect &area = output->extent_hint;
    for (int y = area.top; y < area.bottom; ++y) {
        T *orow = reinterpret_cast<T *>(reinterpret_cast<char *>(output->data) +
                                        static_cast<long>(output->rowbytes) * y);
        for (int x = area.left; x < area.right; ++x) {
            T *out = orow + 4 * x;
            if (x < 0 || y < 0 || x >= input->width || y >= input->height) {
                for (int i = 0; i < 4; ++i) out[i] = T(0);
                continue;
            }
            const T *in = reinterpret_cast<const T *>(reinterpret_cast<const char *>(input->data) +
                                                      static_cast<long>(input->rowbytes) * y) +
                          4 * x;
            RenderPixel<BPC>(ctx, x, y, in, out);
        }
    }
}

PF_Err SmartRender(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra) {
    const int depth = extra->input->bitdepth;
    if (depth != 8 && depth != 16 && depth != 32) return PF_Err_NONE;  // binary: no output

    PF_EffectWorld *input = nullptr;
    PF_EffectWorld *output = nullptr;
    PF_Err err = extra->cb->checkout_layer_pixels(in_data->effect_ref, OLMUV_INPUT, &input);
    const bool inputOut = (err == PF_Err_NONE);
    if (!err) err = extra->cb->checkout_output(in_data->effect_ref, &output);
    if (!err && (!input || !output)) err = PF_Err_BAD_CALLBACK_PARAM;

    Ctx ctx;
    if (!err) ctx.input = ViewOf(input);

    // Interpolation Method (popup value at the current time).
    if (!err) {
        PF_ParamDef popup;
        AEFX_CLR_STRUCT(popup);
        err = PF_CHECKOUT_PARAM(in_data, OLMUV_INTERP, in_data->current_time, in_data->time_step,
                                in_data->time_scale, &popup);
        if (!err) {
            ctx.mode = popup.u.pd.value;
            const PF_Err cin = PF_CHECKIN_PARAM(in_data, &popup);
            if (!err) err = cin;
        }
    }

    // Texture layer: the checked-out PF_LayerDef is used for the whole render.
    PF_ParamDef tex;
    AEFX_CLR_STRUCT(tex);
    bool texOut = false;
    if (!err) {
        err = PF_CHECKOUT_PARAM(in_data, OLMUV_TEXTURE, in_data->current_time, in_data->time_step,
                                in_data->time_scale, &tex);
        texOut = (err == PF_Err_NONE);
        if (texOut) ctx.texture = ViewOf(&tex.u.ld);
    }

    // Trilinear (and binary fall-through for popup values outside 1..2) needs the mip chain.
    if (!err && ctx.mode != OLMUV_NEAREST && ctx.mode != OLMUV_BILINEAR) {
        // ponytail: also accept in_data->sequence_data in case a host leaves
        // out_data's copy unset at SMART_RENDER (the mips are rebuilt locally anyway).
        if (!out_data->sequence_data && !in_data->sequence_data) {
            // Binary: "Trilinear invoked without mipmaps", DISPLAY_ERROR_MESSAGE, error 512.
            out_data->out_flags |= PF_OutFlag_DISPLAY_ERROR_MESSAGE;
            PF_STRNNCPY(out_data->return_msg, "Trilinear invoked without mipmaps",
                        sizeof(out_data->return_msg));
            err = PF_Err_INTERNAL_STRUCT_DAMAGED;
        } else if (ctx.texture.data && ctx.texture.w > 0 && ctx.texture.h > 0) {
            if (depth == 8) ctx.mips = BuildMips<8>(ctx.texture);
            else if (depth == 16) ctx.mips = BuildMips<16>(ctx.texture);
            else ctx.mips = BuildMips<32>(ctx.texture);
        }
    }

    // Smoothness (float at the current time).
    if (!err) {
        PF_ParamDef sm;
        AEFX_CLR_STRUCT(sm);
        err = PF_CHECKOUT_PARAM(in_data, OLMUV_SMOOTHNESS, in_data->current_time,
                                in_data->time_step, in_data->time_scale, &sm);
        if (!err) {
            ctx.smooth = static_cast<float>(sm.u.fs_d.value);
            const PF_Err cin = PF_CHECKIN_PARAM(in_data, &sm);
            if (!err) err = cin;
        }
    }

    if (!err) {
        if (depth == 8) RenderAll<8>(ctx, input, output);
        else if (depth == 16) RenderAll<16>(ctx, input, output);
        else RenderAll<32>(ctx, input, output);
    }

    if (texOut) {
        const PF_Err cin = PF_CHECKIN_PARAM(in_data, &tex);
        if (!err) err = cin;
    }
    if (inputOut) extra->cb->checkin_layer_pixels(in_data->effect_ref, OLMUV_INPUT);
    return err;
}

}  // namespace

PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[],
                  PF_LayerDef *output, void *extra) {
    (void)params;
    (void)output;
    switch (cmd) {
        case PF_Cmd_ABOUT: return About(in_data, out_data);
        case PF_Cmd_GLOBAL_SETUP: return GlobalSetup(out_data);
        case PF_Cmd_PARAMS_SETUP: return ParamsSetup(in_data, out_data);
        case PF_Cmd_SEQUENCE_SETUP:
        case PF_Cmd_SEQUENCE_RESETUP: return SequenceSetup(in_data, out_data);
        case PF_Cmd_SEQUENCE_SETDOWN: return SequenceSetDown(in_data, out_data);
        case PF_Cmd_USER_CHANGED_PARAM:
        case PF_Cmd_UPDATE_PARAMS_UI: return UpdateParamsUI(in_data, out_data);
        case PF_Cmd_SMART_PRE_RENDER: return PreRender(in_data, (PF_PreRenderExtra *)extra);
        case PF_Cmd_SMART_RENDER:
            return SmartRender(in_data, out_data, (PF_SmartRenderExtra *)extra);
        default: return PF_Err_NONE;
    }
}

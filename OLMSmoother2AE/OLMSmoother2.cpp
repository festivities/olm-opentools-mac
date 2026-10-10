/* OLMSmoother2: macOS SmartRender port of the Windows OLM Smoother v2 2.1.
 *
 * Every stage below follows the decompiled Windows binary
 * (entry 0x180009F10, setup 0x180001940, ctx build 0x180004E10, UI
 * 0x1800023A0, smart render 0x180001F10, preprocess 0x180002E90 with
 * converters 0x1800024C0/2600/2700, unpremul 0x180002840, keyers
 * 0x180002A70 (match -> alpha 0) / 0x180002930 (invert list), gamma decode
 * 0x180002BA0, LUTs 0x18000A8E0 (sRGB->linear) / 0x18000A740 (linear->sRGB),
 * lookups 0x180004CD0/4C30, inline 0x180004D70, edge builder 0x18000AE60/
 * ACC0/AED0 with distance 0x18000B370, MLAA dispatcher 0x18000C340,
 * per-pixel glue 0x18000CDA0, blend 0x18000BBD0/C190/ABC0/B1E0/CD30,
 * sample append 0x180010590, ramp 0x1800136F0, end type 0x180010610,
 * stair math 0x180012910/13C80/137C0, stair searches
 * 0x180010C90/10A90/10990/10F00/10DE0/10B90/11010, run searches
 * 0x18000D2F0/D8C0/CFA0/D760/D470/DB10/D5E0/DC90, family mains
 * 0x1800106B0/10770/10820/108E0, appender dispatch 0x18000FCB0/F9B0/FFB0/
 * 102A0, re-classifiers 0x18000E1A0/DF60/E110/DED0/E230/DFF0/E2C0/E080,
 * appenders 0x18000E4F0/E3E0/E460/E350, extended ends
 * 0x18000ED00/E700/EB90/EE70/EFE0/E570/E880/EA10, corners
 * 0x180012CE0/12DA0/13580/13630, run-gated corners
 * 0x180012E60/13020/133B0/13200, stair handlers
 * 0x180011E40/110F0/123A0/11660 (+ bool forms 12100/113C0/12680/11A60),
 * writers 0x180003370/36E0/3990).
 *
 * Documented deviations from the binary:
 *  - Serial loops (the binary uses OpenMP/vcomp).
 *  - The sample list caps at 12; the binary throws out_of_range past 12,
 *    the port stops appending so AE never sees an exception.
 *  - out_flags add PF_OutFlag_SEND_UPDATE_PARAMS_UI (see GlobalSetup).
 *  - The sRGB tables live in function-local statics; the binary stores them
 *    in AEGP memory handles allocated at GlobalSetup (0x180001940).
 *  - The input layer is checked in after SmartRender (the binary relies on
 *    the host reclaiming at frame end); keeps checkout accounting balanced.
 *  - The binary's dirty-rect shrink (0x18000BF70) is skipped: its enable
 *    flag is never set by the live path, so the rect is always the full
 *    image, for which the skip is exactly equivalent.
 */

#include "OLMSmoother2.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

namespace {

constexpr float kInv255 = 0.0039215689f;      // 8-bit decode (0x1800024C0)
constexpr float kInv32768 = 0.000030517578f;  // 16-bit decode (0x180002700)
constexpr float kMatchEps = 0.0019607844f;    // 1/510 color-match epsilon
constexpr float kLumaG = 0.71520001f;         // edge distance luma weights
constexpr float kLumaR = 0.21259999f;
constexpr float kLumaB = 0.0722f;

static_assert(PF_VERSION(OLMS2_MAJOR_VERSION, OLMS2_MINOR_VERSION,
                          OLMS2_BUG_VERSION, PF_Stage_DEVELOP, 0) == 1081344,
              "Smoother2 version must match its PiPL");

AEGP_PluginID g_aegp_id = 0;

void BuildLuts();  // defined below; called at GlobalSetup like the binary
                   // (0x180001940 builds both sRGB LUTs once, single-threaded,
                   // so SmartRender threads never race the lazy init).

PF_Err About(PF_InData *in_data, PF_OutData *out_data) {
    (void)in_data;
    PF_SPRINTF(out_data->return_msg, "%s", OLMS2_ABOUT);
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData *in_data, PF_OutData *out_data) {
    out_data->my_version = PF_VERSION(OLMS2_MAJOR_VERSION, OLMS2_MINOR_VERSION,
                                      OLMS2_BUG_VERSION, PF_Stage_DEVELOP, 0);
    // Binary raw out_flags are 0x02000440 (USE_OUTPUT_EXTENT | PIX_INDEPENDENT |
    // DEEP_COLOR_AWARE). The port adds PF_OutFlag_SEND_UPDATE_PARAMS_UI
    // (-> 0x06000440): per SDK AE_Effect.h that flag is required to receive
    // PF_Cmd_UPDATE_PARAMS_UI, which this effect needs for its parameter
    // visibility logic. Documented deviation from the binary.
    out_data->out_flags = PF_OutFlag_USE_OUTPUT_EXTENT | PF_OutFlag_PIX_INDEPENDENT |
                          PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_SEND_UPDATE_PARAMS_UI;
    // 0x08001400 = SUPPORTS_SMART_RENDER | FLOAT_COLOR_AWARE |
    //              SUPPORTS_THREADED_RENDERING (binary-exact).
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER |
                           PF_OutFlag2_FLOAT_COLOR_AWARE |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    BuildLuts();  // binary builds both sRGB LUTs here (0x180001940)
    if (in_data->pica_basicP) {
        AEGP_SuiteHandler suites(in_data->pica_basicP);
        suites.UtilitySuite3()->AEGP_RegisterWithAEGP((AEGP_GlobalRefcon)0, OLMS2_NAME,
                                                      &g_aegp_id);
    }
    return PF_Err_NONE;
}

PF_Err ParamsSetup(PF_InData *in_data, PF_OutData *out_data) {
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Enable Color Key", 0, PF_ParamFlag_NONE, OLMS2_ID_ENABLE_KEY);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Color Key", 0xFF, 0xFF, 0xFF, OLMS2_ID_KEY_COLOR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Invert Color Key", 0, PF_ParamFlag_NONE, OLMS2_ID_INVERT_KEY);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Smoothness", 0, 100, 0, 100, 100, OLMS2_ID_SMOOTHNESS);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Extra Smooth", 0, 100, 0, 100, 0, OLMS2_ID_EXTRA_SMOOTH);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Smooth Range", 0, 100, 0, 100, 2, OLMS2_ID_SMOOTH_RANGE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Smoother Version", 2, 2, "v1|v2", PF_ParamFlag_NONE,
                  OLMS2_ID_VERSION);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Gamma Correction", 3, OLMS2_GAMMA_NONE, "None|Gamma Colors|All Colors",
                  PF_ParamFlag_SUPERVISE, OLMS2_ID_GAMMA_MODE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Gamma Value", 1.0, 2.4, 1.0, 2.4, 2.4, 2,
                         PF_ValueDisplayFlag_NONE, PF_ParamFlag_NONE,
                         OLMS2_ID_GAMMA_VALUE);

    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_SUPERVISE;
    PF_ADD_SLIDER("Number of Gamma Colors", 0, OLMS2_MAX_GAMMA_COLORS, 0, 5, 1,
                  OLMS2_ID_GAMMA_COUNT);

    for (int i = 0; i < OLMS2_MAX_GAMMA_COLORS; ++i) {
        AEFX_CLR_STRUCT(def);
        PF_ADD_COLOR("Gamma Color", 0, 0, 0, OLMS2_ID_GAMMA_COLOR + i);
    }

    out_data->num_params = OLMS2_NUM_PARAMS;
    return PF_Err_NONE;
}

// --- UPDATE_PARAMS_UI (0x1800023A0 + 0x180005DE0/5FA0) -----------------------

PF_Err SetParamDisabled(AEFX_SuiteScoper<PF_ParamUtilsSuite3> &suite,
                        PF_InData *in_data, PF_ParamIndex position,
                        const PF_ParamDef *current, bool disabled) {
    if (!in_data || !current) return PF_Err_BAD_CALLBACK_PARAM;
    // sub_180005DE0: copy the host's live definition (never a zeroed struct)
    // and toggle only PF_PUI_DISABLED.
    PF_ParamDef def = *current;
    if (disabled) def.ui_flags |= PF_PUI_DISABLED;
    else def.ui_flags &= ~((A_long)PF_PUI_DISABLED);
    return suite->PF_UpdateParamUI(in_data->effect_ref, position, &def);
}

PF_Err SetStreamVisible(AEGP_SuiteHandler &suites, AEGP_EffectRefH effectH,
                        PF_ParamIndex position, bool visible) {
    // sub_180005FA0: AEGP_SetDynamicStreamFlag(HIDDEN, undoable=false, !visible).
    AEGP_StreamRefH streamH = nullptr;
    PF_Err err = suites.StreamSuite7()->AEGP_GetNewEffectStreamByIndex(
        g_aegp_id, effectH, position, &streamH);
    if (!err && streamH) {
        err = suites.DynamicStreamSuite3()->AEGP_SetDynamicStreamFlag(
            streamH, AEGP_DynStreamFlag_HIDDEN, false, visible ? FALSE : TRUE);
        suites.StreamSuite7()->AEGP_DisposeStream(streamH);
    }
    return err;
}

PF_Err UpdateParamsUI(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[]) {
    if (!in_data || !out_data || !params) return PF_Err_BAD_CALLBACK_PARAM;
    if (!in_data->pica_basicP || !g_aegp_id) return PF_Err_NONE;

    const bool enable_off = !(params[OLMS2_ENABLE_KEY] &&
                              params[OLMS2_ENABLE_KEY]->u.bd.value != 0);
    const A_long mode = params[OLMS2_GAMMA_MODE] ? params[OLMS2_GAMMA_MODE]->u.pd.value : 1;
    const A_long count = params[OLMS2_GAMMA_COUNT] ? params[OLMS2_GAMMA_COUNT]->u.sd.value : 0;

    AEFX_SuiteScoper<PF_ParamUtilsSuite3> param_utils(
        in_data, kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3, out_data);

    // Disable truth table (0x1800023A0): Color Key (pos 2) and Invert (pos 3)
    // when Enable is off; Gamma Value when mode == None; Count when
    // mode != Gamma Colors.
    PF_Err err = SetParamDisabled(param_utils, in_data, OLMS2_KEY_COLOR,
                                  params[OLMS2_KEY_COLOR], enable_off);
    if (!err) err = SetParamDisabled(param_utils, in_data, OLMS2_INVERT_KEY,
                                     params[OLMS2_INVERT_KEY], enable_off);
    if (!err) err = SetParamDisabled(param_utils, in_data, OLMS2_GAMMA_VALUE,
                                     params[OLMS2_GAMMA_VALUE], mode == OLMS2_GAMMA_NONE);
    if (!err) err = SetParamDisabled(param_utils, in_data, OLMS2_GAMMA_COUNT,
                                     params[OLMS2_GAMMA_COUNT], mode != OLMS2_GAMMA_COLORS);

    // Hide the gamma pickers (0x180005FA0): visible iff mode == Gamma Colors
    // and i < count.
    if (!err) {
        AEGP_SuiteHandler suites(in_data->pica_basicP);
        AEGP_LayerH layerH = nullptr;
        err = suites.PFInterfaceSuite1()->AEGP_GetEffectLayer(in_data->effect_ref, &layerH);
        if (err || !layerH) return err ? err : PF_Err_BAD_CALLBACK_PARAM;
        AEGP_EffectRefH effectH = nullptr;
        err = suites.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(
            g_aegp_id, in_data->effect_ref, &effectH);
        if (err || !effectH) {
            if (effectH) suites.EffectSuite5()->AEGP_DisposeEffect(effectH);
            return err ? err : PF_Err_BAD_CALLBACK_PARAM;
        }
        for (int i = 0; i < OLMS2_MAX_GAMMA_COLORS && !err; ++i) {
            err = SetStreamVisible(suites, effectH, OLMS2_GAMMA_COLOR_0 + i,
                                   mode == OLMS2_GAMMA_COLORS && i < count);
        }
        suites.EffectSuite5()->AEGP_DisposeEffect(effectH);
    }
    return err;
}

// --- Parameter checkout ------------------------------------------------------

template <typename ReadValue>
PF_Err checkout_value(PF_InData *in_data, PF_ParamIndex position, ReadValue read_value) {
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    PF_Err err = PF_CHECKOUT_PARAM(in_data, position, in_data->current_time, in_data->time_step,
                                   in_data->time_scale, &def);
    if (err) return err;
    read_value(def);
    return PF_CHECKIN_PARAM(in_data, &def);
}

PF_Err CheckoutParams(PF_InData *in_data, PF_OutData *out_data, A_long bitdepth,
                      OLMS2Params *p) {
    std::memset(p, 0, sizeof(*p));
    PF_Err err;

#define OLMS2_READ(POS, BODY) \
    do { \
        err = checkout_value(in_data, (POS), [&](const PF_ParamDef &def) { BODY; }); \
        if (err) return err; \
    } while (0)

    OLMS2_READ(OLMS2_ENABLE_KEY, p->enable_key = def.u.bd.value != 0);
    OLMS2_READ(OLMS2_INVERT_KEY, p->invert_key = def.u.bd.value != 0);
    OLMS2_READ(OLMS2_SMOOTHNESS, p->smoothness = def.u.sd.value);
    OLMS2_READ(OLMS2_EXTRA_SMOOTH, p->extra_smooth = def.u.sd.value);
    OLMS2_READ(OLMS2_SMOOTH_RANGE, p->smooth_range = def.u.sd.value);
    OLMS2_READ(OLMS2_VERSION, p->v1 = def.u.pd.value == 1);
    OLMS2_READ(OLMS2_GAMMA_MODE, p->gamma_mode = def.u.pd.value);
    OLMS2_READ(OLMS2_GAMMA_VALUE, p->gamma_value = (float)def.u.fs_d.value);
    OLMS2_READ(OLMS2_GAMMA_COUNT, p->gamma_count = def.u.sd.value);

    if (p->enable_key) {
        AEFX_SuiteScoper<PF_ColorParamSuite1> color_suite(
            in_data, kPFColorParamSuite, kPFColorParamSuiteVersion1, out_data);
        PF_ParamDef def;
        AEFX_CLR_STRUCT(def);
        err = PF_CHECKOUT_PARAM(in_data, OLMS2_KEY_COLOR, in_data->current_time,
                                in_data->time_step, in_data->time_scale, &def);
        if (err) return err;
        PF_PixelFloat f;
        err = color_suite->PF_GetFloatingPointColorFromColorDef(in_data->effect_ref, &def, &f);
        const PF_Err checkin_err = PF_CHECKIN_PARAM(in_data, &def);
        if (err) return err;
        if (checkin_err) return checkin_err;
        p->key_color[0] = f.red;
        p->key_color[1] = f.green;
        p->key_color[2] = f.blue;
    }
    if (p->gamma_mode == OLMS2_GAMMA_COLORS) {
        if (p->gamma_count < 0 || p->gamma_count > OLMS2_MAX_GAMMA_COLORS)
            return PF_Err_BAD_CALLBACK_PARAM;
        AEFX_SuiteScoper<PF_ColorParamSuite1> color_suite(
            in_data, kPFColorParamSuite, kPFColorParamSuiteVersion1, out_data);
        for (int i = 0; i < p->gamma_count; ++i) {
            PF_ParamDef def;
            AEFX_CLR_STRUCT(def);
            err = PF_CHECKOUT_PARAM(in_data, OLMS2_GAMMA_COLOR_0 + i, in_data->current_time,
                                    in_data->time_step, in_data->time_scale, &def);
            if (err) return err;
            PF_PixelFloat f;
            err = color_suite->PF_GetFloatingPointColorFromColorDef(in_data->effect_ref, &def, &f);
            const PF_Err checkin_err = PF_CHECKIN_PARAM(in_data, &def);
            if (err) return err;
            if (checkin_err) return checkin_err;
            p->gamma_colors[i][0] = f.red;
            p->gamma_colors[i][1] = f.green;
            p->gamma_colors[i][2] = f.blue;
        }
    }

#undef OLMS2_READ

    p->bitdepth = bitdepth;
    return PF_Err_NONE;
}

// --- sRGB tables (0x18000A8E0 decode / 0x18000A740 encode, N = 10000) --------

constexpr int kLutN = 10000;

struct SrgbLut {
    bool built;
    float table[kLutN];
};

SrgbLut g_decode_lut;  // sRGB -> linear
SrgbLut g_encode_lut;  // linear -> sRGB

void BuildLuts() {
    // Grid position is a single-precision divide in the binary (0x18000A8E0/A740:
    // v7 = (float)((float)i / 9999.0f)), not a double divide.
    if (!g_decode_lut.built) {
        for (int i = 0; i < kLutN; ++i) {
            const double x = (double)((float)i / (float)(kLutN - 1));
            double v;
            if (x > 0.0) {
                if (x < 1.0) {
                    if (x >= 0.04045)
                        v = std::pow((x + 0.055) / 1.055, 2.4);
                    else
                        v = x / 12.92;
                } else {
                    v = 1.0;
                }
            } else {
                v = 0.0;
            }
            g_decode_lut.table[i] = (float)v;
        }
        g_decode_lut.built = true;
    }
    if (!g_encode_lut.built) {
        for (int i = 0; i < kLutN; ++i) {
            const double x = (double)((float)i / (float)(kLutN - 1));
            double v;
            if (x > 0.0) {
                if (x < 1.0) {
                    if (x >= 0.0031308)
                        v = std::pow(x, 0.4166666666666667) * 1.055 - 0.055;
                    else
                        v = x * 12.92;
                } else {
                    v = 1.0;
                }
            } else {
                v = 0.0;
            }
            g_encode_lut.table[i] = (float)v;
        }
        g_encode_lut.built = true;
    }
}

// 0x180004CD0: decode LUT lookup with double lerp; 0x180004D70 inline fallback.
float SrgbDecode(float x) {
    if (!(x > 0.0f)) return 0.0f;
    if (!(x < 1.0f)) return 1.0f;
    if (g_decode_lut.built) {
        const double t = (double)(kLutN - 1) * (double)x;
        const int i = (int)t;
        const double frac = t - (double)i;
        const double lo = (double)g_decode_lut.table[i];
        const double hi = (double)g_decode_lut.table[i + 1];
        return (float)((1.0 - frac) * lo + frac * hi);
    }
    const double v = (double)x;
    if (v >= 0.04045) return (float)std::pow(v * 0.9478672985781991 + 0.05213270142180095, 2.4);
    return (float)(v * 0.07739938080495357);
}

// 0x180004C30 / 0x180004D70.
float SrgbEncode(float x) {
    if (!(x > 0.0f)) return 0.0f;
    if (!(x < 1.0f)) return 1.0f;
    if (g_encode_lut.built) {
        const double t = (double)(kLutN - 1) * (double)x;
        const int i = (int)t;
        const double frac = t - (double)i;
        const double lo = (double)g_encode_lut.table[i];
        const double hi = (double)g_encode_lut.table[i + 1];
        return (float)(frac * hi + (1.0 - frac) * lo);
    }
    const double v = (double)x;
    if (v >= 0.0031308) return (float)(std::pow(v, 0.4166666666666667) * 1.055 - 0.055);
    return (float)(v * 12.92);
}

// --- Working buffers ---------------------------------------------------------

struct CanvasDesc {
    float *data;   // R,G,B,A float4 per pixel
    int w;
    int h;
    int stride;    // bytes per row
    const float *Pixel(int x, int y) const {
        return (const float *)((const char *)data + (size_t)stride * (size_t)y + (size_t)x * 16);
    }
    float *Pixel(int x, int y) {
        return (float *)((char *)data + (size_t)stride * (size_t)y + (size_t)x * 16);
    }
};

struct EdgeDesc {
    A_u_char *data;  // 4 bytes per pixel: b0 left, b1 up, b2 up-left, b3 up-right
    int w;
    int h;
    int stride;      // bytes per row
    // Byte != 0 (0xFF) <=> edge present; byte == 0 <=> edge absent.
    A_u_char Byte(int x, int y, int b) const {
        return data[(size_t)stride * (size_t)y + (size_t)x * 4 + b];
    }
    bool Present(int x, int y, int b) const { return Byte(x, y, b) != 0; }
};

class ScopedWorld {
public:
    ScopedWorld() : suiteP(nullptr), effect_ref(nullptr), created(false) {
        AEFX_CLR_STRUCT(world);
    }
    ~ScopedWorld() {
        if (created && suiteP) suiteP->PF_DisposeWorld(effect_ref, &world);
    }
    PF_Err Create(PF_InData *in_data, const PF_WorldSuite2 *suite, A_long width,
                  A_long height, PF_PixelFormat format) {
        suiteP = suite;
        effect_ref = in_data->effect_ref;
        const PF_Err err = suite->PF_NewWorld(effect_ref, width, height, TRUE, format,
                                              &world);
        created = err == PF_Err_NONE;
        return err;
    }
    PF_EffectWorld *get() { return &world; }

private:
    ScopedWorld(const ScopedWorld &);
    ScopedWorld &operator=(const ScopedWorld &);
    PF_EffectWorld world;
    const PF_WorldSuite2 *suiteP;
    PF_ProgPtr effect_ref;
    bool created;
};

// --- Preprocess (0x180002E90) -------------------------------------------------

// 0x1800024C0 / 0x180002700 / 0x180002600: source A,R,G,B -> canvas R,G,B,A.
// The decode factor is selected by pixel TYPE (PF_Pixel16 is 8 bytes wide,
// so sizeof() cannot discriminate the 8-bit path).

template <typename T> struct PixelDecode;
template <> struct PixelDecode<PF_Pixel> {
    static constexpr float kValue = kInv255;
};
template <> struct PixelDecode<PF_Pixel16> {
    static constexpr float kValue = kInv32768;
};
template <> struct PixelDecode<PF_PixelFloat> {
    static constexpr float kValue = 1.0f;
};

template <typename Pixel>
void ConvertToCanvas(const PF_EffectWorld &src, CanvasDesc &dst) {
    const float decode = PixelDecode<Pixel>::kValue;
    for (int y = 0; y < dst.h; ++y) {
        const Pixel *row = (const Pixel *)((const char *)src.data + (size_t)src.rowbytes * (size_t)y);
        for (int x = 0; x < dst.w; ++x) {
            const Pixel &p = row[x];
            float *out = dst.Pixel(x, y);
            out[0] = (float)p.red * decode;
            out[1] = (float)p.green * decode;
            out[2] = (float)p.blue * decode;
            out[3] = (float)p.alpha * decode;
        }
    }
}

// 0x180002A70: key (match -> alpha 0); 0x180002930: invert (non-match -> 0).
bool ColorMatch(const float *px, const float *key) {
    return fabsf(px[0] - key[0]) < kMatchEps &&
           fabsf(px[1] - key[1]) < kMatchEps &&
           fabsf(px[2] - key[2]) < kMatchEps;
}

void ApplyKey(CanvasDesc &c, const OLMS2Params &p) {
    if (!p.enable_key) return;
    if (p.invert_key) {
        for (int y = 0; y < c.h; ++y) {
            for (int x = 0; x < c.w; ++x) {
                float *px = c.Pixel(x, y);
                if (!ColorMatch(px, p.key_color)) px[3] = 0.0f;
            }
        }
    } else {
        for (int y = 0; y < c.h; ++y) {
            for (int x = 0; x < c.w; ++x) {
                float *px = c.Pixel(x, y);
                if (ColorMatch(px, p.key_color)) px[3] = 0.0f;
            }
        }
    }
}

void ApplyGammaDecode(CanvasDesc &c, const OLMS2Params &p) {
    if (p.v1) return;
    BuildLuts();
    for (int y = 0; y < c.h; ++y) {
        for (int x = 0; x < c.w; ++x) {
            float *px = c.Pixel(x, y);
            px[0] = SrgbDecode(px[0]);
            px[1] = SrgbDecode(px[1]);
            px[2] = SrgbDecode(px[2]);
        }
    }
}

// --- Edge distance (0x18000B370) ---------------------------------------------

float EdgeDistance(const float *a, const float *b) {
    if (a[3] == 0.0f && b[3] == 0.0f) return 0.0f;
    const float dg = fabsf(a[1] - b[1]);
    const float dr = fabsf(a[0] - b[0]);
    const float mx_gr = dg <= dr ? dr : dg;
    float m = fabsf(a[2] - b[2]);
    if (m <= mx_gr) m = mx_gr;
    const float la = (a[1] * kLumaG + a[0] * kLumaR) + a[2] * kLumaB;
    const float lb = (b[1] * kLumaG + b[0] * kLumaR) + b[2] * kLumaB;
    return fmaxf(m, fabsf(la - lb)) + fabsf(a[3] - b[3]);
}

// --- Edge map build (0x18000AED0) --------------------------------------------

void BuildEdges(const CanvasDesc &c, EdgeDesc &e, int smooth_range) {
    const float thr = (float)((double)(float)smooth_range / 100.0) + 0.001f;
    for (int y = 0; y < c.h; ++y) {
        for (int x = 0; x < c.w; ++x) {
            const float *p = c.Pixel(x, y);
            bool b0 = false, b1 = false, b2 = false, b3 = false;
            if (x >= 1) b0 = EdgeDistance(p, c.Pixel(x - 1, y)) >= thr;
            if (y >= 1) {
                b1 = EdgeDistance(p, c.Pixel(x, y - 1)) >= thr;
                if (x >= 1) b2 = EdgeDistance(p, c.Pixel(x - 1, y - 1)) >= thr;
                if (x + 1 < c.w - 1) b3 = EdgeDistance(p, c.Pixel(x + 1, y - 1)) >= thr;
            }
            A_u_char *out = e.data + (size_t)e.stride * (size_t)y + (size_t)x * 4;
            // Binary negates the presence flag: present -> 0xFF, absent -> 0.
            out[0] = b0 ? 0xFF : 0;
            out[1] = b1 ? 0xFF : 0;
            out[2] = b2 ? 0xFF : 0;
            out[3] = b3 ? 0xFF : 0;
        }
    }
}

// --- MLAA (0x18000C340 and helpers) -------------------------------------------

constexpr int kMaxSamples = 12;

struct MlaaCtx {
    const CanvasDesc *canvas;
    const EdgeDesc *edge;
    int x;
    int y;
    float s;                        // Smoothness / 100
    float es;                       // Extra Smooth / 100
    float samples[kMaxSamples][5];  // {R,G,B,A,weight} records (0x180010590)
    int count;                      // the qword at ctx+304
};

// 0x180010590. The binary throws out_of_range past 12 samples; the port stops
// appending instead (documented deviation) so AE never sees an exception.
void AppendSample(MlaaCtx &c, int x, int y, float weight) {
    if (c.count >= kMaxSamples) return;
    const float *p = c.canvas->Pixel(x, y);
    float *rec = c.samples[c.count];
    rec[0] = p[0];
    rec[1] = p[1];
    rec[2] = p[2];
    rec[3] = p[3];
    rec[4] = weight;
    ++c.count;
}

// 0x18000CD30: divide every appended weight by the final count.
void NormalizeSamples(MlaaCtx &c) {
    if (c.count == 0) return;
    const float n = (float)c.count;
    for (int i = 0; i < c.count; ++i) c.samples[i][4] /= n;
}

// 0x180010610: end-type classifier.
int EndType(int a1, int a2, int a3, int a4, int a5) {
    switch (a1 + 2 * (a3 + 2 * a2)) {
        case 0:
        case 1:
            return 0;
        case 2:
            return 1;
        case 3:
            return 4;
        case 4:
            return 2;
        case 5:
            return 3;
        case 6:
            return 5;
        default: {  // case 7
            const int v6 = a4 + 2 * a5;
            if (v6 == 0) return 6;
            const int v7 = v6 - 1;
            if (v7 == 0) return 7;
            const int v8 = v7 - 1;
            if (v8 == 0) return 8;
            if (v8 == 1) return 9;
            return 6;
        }
    }
}

// 0x1800136F0: ramp weight. Float math throughout (verified in disasm).
float Ramp(float L, int d, float h) {
    if (L == 0.0f) return 0.0f;
    const float fd = (float)d;
    if (fd >= L || h == 0.0f) return 0.0f;
    const float t = fd / L;
    const float a = (1.0f - t) * h;
    if (L < fd + 1.0f) return (L - fd) * a * 0.5f;
    const float t2 = (fd + 1.0f) / L;
    return ((1.0f - t2) * h + a) * 0.5f;
}

// 0x1800137C0: coverage weights of a diagonal line at pixel coordinate n.
// Faithful translation of the 186-line decompile.
void StairWeights137C0(float out[2], const float p1[2], const float p2[2], int n) {
    const float v5 = p1[1];
    const float v6 = p2[1] - v5;
    const float v7 = p1[0];
    const float v8 = p2[0];
    const float v9 = p2[0] - p1[0];
    if (v6 == 0.0f || v9 == 0.0f) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        return;
    }
    float v10 = 1.0f;
    const float v12 = (float)n + 1.0f;
    const float v13 = v12 + 1.0f;
    const float v14 = ((v12 - v5) * v9) / v6 + v7;
    const float v15 = ((v12 - v7) * v6) / v9 + v5;
    const float v16 = ((v12 + 1.0f - v5) * v9) / v6 + v7;
    const float v17 = ((v12 + 1.0f - v7) * v6) / v9 + v5;
    const float v18 = ((v12 - 1.0f - v5) * v9) / v6 + v7;

    int v19 = (int)(float)(v15 - v12);
    if ((float)(v15 - v12) < 0.0f)
        v19 -= (float)-(float)(v15 - v12) > (float)(int)(float)-(float)(v15 - v12) ? 1 : 0;
    int v21;
    if (v19 > -2) {
        if (v19 == -1) v21 = 2;
        else v21 = (v19 == 0) ? 1 : 0;
    } else {
        v21 = 3;
    }
    int v22 = (int)(float)(v17 - v12);
    if ((float)(v17 - v12) < 0.0f)
        v22 -= (float)-(float)(v17 - v12) > (float)(int)(float)-(float)(v17 - v12) ? 1 : 0;
    int v20 = 3;
    if (v22 > -2) {
        if (v22 == -1) v20 = 2;
        else v20 = (v22 == 0) ? 1 : 0;
    }
    int v23 = (int)v15;
    if (v15 < 0.0f)
        v23 -= (float)-v15 > (float)(int)(float)-v15 ? 1 : 0;
    const float v24 = (float)v23;
    int v25 = (int)v17;
    const float v26 = v15 - v24;
    if (v17 < 0.0f) {
        const bool v11 = (float)-v17 > (float)(int)(float)-v17;
        v25 -= v11 ? 1 : 0;
    }
    const float v27 = v17 - (float)v25;

    float v28 = 1.0f;
    float v29 = 0.0f;
    float v30 = 0.0f;
    if (v21 != 3) {
        if (v21 == 2) {
            switch (v20) {
                case 2:
                    v10 = 0.0f;
                    v28 = 1.0f - (float)((v27 + v26) * 0.5f);
                    goto finish;
                case 1:
                    v28 = ((v14 - v12) * (1.0f - v26)) * 0.5f;
                    v10 = ((v13 - v14) * v27) * 0.5f;
                    goto finish;
                case 0:
                    v28 = ((v14 - v12) * (1.0f - v26)) * 0.5f;
                    goto label29;
                default:
                    break;
            }
            v29 = (v18 - v12) * v26;
            v30 = 1.0f;
            v10 = 0.0f;
        } else {
            if (v21 == 1) {
                switch (v20) {
                    case 2:
                        v28 = ((v13 - v14) * (1.0f - v27)) * 0.5f;
                        v10 = ((v14 - v12) * v26) * 0.5f;
                        goto finish;
                    case 1:
                        v28 = 0.0f;
                        v10 = (v27 + v26) * 0.5f;
                        goto finish;
                    case 0:
                        v28 = 0.0f;
                        v10 = 1.0f - ((v16 - v12) * (1.0f - v26)) * 0.5f;
                        goto finish;
                    default:
                        break;
                }
                v10 = ((v14 - v12) * v26) * 0.5f;
            } else {
                switch (v20) {
                    case 2:
                        v28 = ((v13 - v16) * (1.0f - v27)) * 0.5f;
                        v10 = ((v14 + v16) * 0.5f) - v12;
                        goto finish;
                    case 1:
                        v28 = 0.0f;
                        v10 = 1.0f - ((v12 - v16) * (1.0f - v27)) * 0.5f;
                        goto finish;
                    case 0:
                        v28 = 0.0f;
                        goto finish;
                    default:
                        break;
                }
                v10 = ((v14 + v16) * 0.5f) - v12;
            }
            v29 = v18 + v14;
            v30 = v12 + 1.0f;
        }
        v28 = v30 - (v29 * 0.5f);
        goto finish;
    }
    switch (v20) {
        case 2:
            v10 = 1.0f - ((v13 - v18) * v27) * 0.5f;
            break;
        case 1:
            v28 = ((v18 + v14) * 0.5f) - v12;
            v10 = ((v13 - v14) * v27) * 0.5f;
            goto finish;
        case 0:
            v28 = ((v18 + v14) * 0.5f) - v12;
        label29:
            v10 = v13 - ((v14 + v16) * 0.5f);
            goto finish;
        default:
            break;
    }
    v28 = v10;
    v10 = 0.0f;
finish:
    if (v7 > v12) v28 = 0.0f;
    if (v13 > v8) v10 = 0.0f;
    out[0] = v28;
    out[1] = v10;
}

// 0x180013C80: intersect two lines, clip to the pixel band, call 137C0.
void StairWeights13C80(float out[2], const float a2[2], const float a3[2],
                       const float a4[2], const float a28[2], int n) {
    const float v29 = a2[0];
    const float v30 = a2[1];
    const float v32 = a3[0] - a2[0];
    const float v33 = a3[1] - a2[1];
    const float v34 = a4[0];
    const float v35 = a28[0];
    const float v36 = a4[1];
    const float v37 = a28[0] - a4[0];
    const float v38 = a28[1];
    const float v39 = a28[1] - a4[1];
    const float v40 = (float)n + 1.0f;
    const float v41 = v40 + 1.0f;
    const float v42 = ((a2[0] / v32) * v33 - v30 -
                       ((a4[0] / v37) * v39 - v36)) /
                      (v33 / v32 - v39 / v37);
    float p1[2], p2[2];
    p1[0] = v40;
    p1[1] = (v42 <= v40) ? ((v40 - v34) / v37) * v39 + v36
                         : ((v40 - v29) / v32) * v33 + v30;
    p2[0] = v41;
    p2[1] = (v42 <= v41) ? ((v41 - v34) / v37) * v39 + v36
                         : ((v41 - v29) / v32) * v33 + v30;
    // Endpoint swap-backs from the binary:
    //   if (P1.x <= n+1 && n+2 > P4_old.x) -> use P4_old as the second point
    //   else if (P1.x > n+1)               -> use P1 as the first point
    if (v29 <= v40) {
        if (v41 > v35) {
            p2[0] = v35;
            p2[1] = v38;
        }
    } else {
        p1[0] = v29;
        p1[1] = v30;
    }
    StairWeights137C0(out, p1, p2, n);
}

// 0x180012910: stair weight dispatcher. h = (float)a2; case = a5 + 4*a4;
// k (a3) is the pixel coordinate passed through to 13C80/137C0.
void StairWeights12910(float out[2], int h_int, int k, int a4, int a5) {
    const int which = a5 + 4 * a4;
    const float h = (float)h_int;
    switch (which) {
        case 0: {
            const float p1[2] = {1.5f, 1.0f};
            const float p2[2] = {h + 0.5f, h};
            StairWeights137C0(out, p1, p2, k);
            return;
        }
        case 1: {
            const float p1[2] = {1.5f, 1.0f};
            const float p2[2] = {h + 1.0f, h};
            StairWeights137C0(out, p1, p2, k);
            return;
        }
        case 2: {
            const float l1[2][2] = {{1.5f, 1.0f}, {h + 2.0f, h + 1.0f}};
            const float l2[2][2] = {{1.0f, 0.0f}, {h + 1.0f, h + 0.5f}};
            StairWeights13C80(out, l1[0], l1[1], l2[0], l2[1], k);
            return;
        }
        case 3: {
            const float l1[2][2] = {{1.5f, 1.0f}, {h + 2.0f, h + 1.0f}};
            const float l2[2][2] = {{1.0f, 0.0f}, {h + 1.0f, h + 1.0f}};
            StairWeights13C80(out, l1[0], l1[1], l2[0], l2[1], k);
            return;
        }
        case 4: {
            const float p1[2] = {1.0f, 1.0f};
            const float p2[2] = {h + 0.5f, h};
            StairWeights137C0(out, p1, p2, k);
            return;
        }
        case 5: {
            const float p1[2] = {1.0f, 1.0f};
            const float p2[2] = {h + 1.0f, h};
            StairWeights137C0(out, p1, p2, k);
            return;
        }
        case 6: {
            const float l1[2][2] = {{1.0f, 1.0f}, {h + 2.0f, h + 1.0f}};
            const float l2[2][2] = {{1.0f, 0.0f}, {h + 1.0f, h + 0.5f}};
            StairWeights13C80(out, l1[0], l1[1], l2[0], l2[1], k);
            return;
        }
        case 7: {
            const float l1[2][2] = {{1.0f, 1.0f}, {h + 2.0f, h + 1.0f}};
            const float l2[2][2] = {{1.0f, 0.0f}, {h + 1.0f, h + 1.0f}};
            StairWeights13C80(out, l1[0], l1[1], l2[0], l2[1], k);
            return;
        }
        case 8: {
            const float l1[2][2] = {{1.0f, 0.5f}, {h + 1.0f, h + 1.0f}};
            const float l2[2][2] = {{0.0f, 0.0f}, {h + 0.5f, h}};
            StairWeights13C80(out, l1[0], l1[1], l2[0], l2[1], k);
            return;
        }
        case 9: {
            const float l1[2][2] = {{1.0f, 0.5f}, {h + 1.0f, h + 1.0f}};
            const float l2[2][2] = {{0.0f, 0.0f}, {h + 1.0f, h}};
            StairWeights13C80(out, l1[0], l1[1], l2[0], l2[1], k);
            return;
        }
        case 10: {
            const float p1[2] = {1.0f, 0.5f};
            const float p2[2] = {h + 1.0f, h + 0.5f};
            StairWeights137C0(out, p1, p2, k);
            return;
        }
        case 11: {
            const float p1[2] = {1.0f, 0.5f};
            const float p2[2] = {h + 1.0f, h + 1.0f};
            StairWeights137C0(out, p1, p2, k);
            return;
        }
        case 12: {
            const float l1[2][2] = {{1.0f, 0.0f}, {h + 1.0f, h + 1.0f}};
            const float l2[2][2] = {{0.0f, 0.0f}, {h + 0.5f, h}};
            StairWeights13C80(out, l1[0], l1[1], l2[0], l2[1], k);
            return;
        }
        case 13: {
            const float l1[2][2] = {{1.0f, 0.0f}, {h + 1.0f, h + 1.0f}};
            const float l2[2][2] = {{0.0f, 0.0f}, {h + 1.0f, h}};
            StairWeights13C80(out, l1[0], l1[1], l2[0], l2[1], k);
            return;
        }
        case 14: {
            const float p1[2] = {1.0f, 0.0f};
            const float p2[2] = {h + 1.0f, h + 0.5f};
            StairWeights137C0(out, p1, p2, k);
            return;
        }
        case 15: {
            const float p1[2] = {1.0f, 0.0f};
            const float p2[2] = {h + 1.0f, h + 1.0f};
            StairWeights137C0(out, p1, p2, k);
            return;
        }
        default:
            out[0] = 0.0f;
            out[1] = 0.0f;
            return;
    }
}

// --- Run searches (0x18000D2F0/D8C0/CFA0/D760/D470/DB10/D5E0/DC90) ------------
// Each returns {x, y, type}. Byte != 0 <=> edge present.

struct SearchResult {
    int x;
    int y;
    int type;
};

// Bounds-checked edge read: the binary guards every EndType byte lookup and
// substitutes 0 outside the image.
bool PresentIn(const EdgeDesc &e, int x, int y, int b) {
    if (x < 0 || x >= e.w || y < 0 || y >= e.h) return false;
    return e.Present(x, y, b);
}

// 0x18000D2F0: family A LEFT search on row y+1.
SearchResult SearchD2F0(const EdgeDesc &e, int x, int y) {
    const int L = y + 1;
    if (x < 0 || x >= e.w || L < 0 || L >= e.h) return {x, L, 0};
    int i = x;
    if (x >= 1) {
        int count = x;
        for (;;) {
            if (!e.Present(i, L, 1)) { ++i; break; }              // run edge absent -> i+1
            if (e.Present(i, L, 0) || e.Present(i, y, 0)) break;  // blocking vertical -> i
            --i;
            if (--count < 1) break;                                // x hit 0 -> i
        }
    }
    const int ir = i;
    const int a1 = (ir >= 1 && e.Present(ir - 1, L, 1)) ? 1 : 0;
    const int a2 = e.Present(ir, y, 0) ? 1 : 0;
    const int a3 = e.Present(ir, L, 0) ? 1 : 0;
    const int a4 = e.Present(ir, L, 2) ? 1 : 0;
    const int a5 = (ir >= 1 && e.Present(ir - 1, L, 3)) ? 1 : 0;
    return {ir, y, EndType(a1, a2, a3, a4, a5)};
}

// 0x18000D8C0: family A RIGHT search on row y+1.
SearchResult SearchD8C0(const EdgeDesc &e, int x, int y) {
    const int L = y + 1;
    if (x < 0 || x >= e.w || L < 0 || L >= e.h) return {x, L, 0};
    int i = x + 1;
    if (i < e.w - 1) {
        while (i < e.w - 1) {
            if (!e.Present(i, L, 1)) break;
            if (e.Present(i, L, 0) || e.Present(i, y, 0)) break;
            ++i;
        }
    }
    const int ir = i - 1;
    // Binary reads the EndType bytes at the STOP pixel (i, L)/(i, y) — one past
    // the returned end — with a5 = b3(ir, L); OOB reads are 0.
    const int a1 = PresentIn(e, i, L, 1) ? 1 : 0;
    const int a2 = PresentIn(e, i, L, 0) ? 1 : 0;
    const int a3 = PresentIn(e, i, y, 0) ? 1 : 0;
    const int a4 = PresentIn(e, i, L, 2) ? 1 : 0;
    const int a5 = PresentIn(e, ir, L, 3) ? 1 : 0;
    return {ir, y, EndType(a1, a2, a3, a4, a5)};
}

// 0x18000CFA0: family B UP search on column x.
SearchResult SearchCFA0(const EdgeDesc &e, int x, int y) {
    if (x < 0 || x >= e.w || y < 0 || y >= e.h) return {x, y, 0};
    int yr = y;
    if (y >= 1) {
        int count = y;
        for (;;) {
            if (!e.Present(x, yr, 0)) { ++yr; break; }                  // run absent -> y'+1
            if (e.Present(x, yr, 1) || e.Present(x - 1, yr, 1)) break;  // blocking
            --yr;
            if (--count < 1) break;
        }
    }
    const int a1 = (yr >= 1 && e.Present(x, yr - 1, 0)) ? 1 : 0;
    const int a2 = e.Present(x, yr, 1) ? 1 : 0;
    const int a3 = e.Present(x - 1, yr, 1) ? 1 : 0;
    const int a4 = e.Present(x - 1, yr, 3) ? 1 : 0;
    const int a5 = e.Present(x, yr, 2) ? 1 : 0;
    return {x, yr, EndType(a1, a2, a3, a4, a5)};
}

// 0x18000D760: family B DOWN search on column x.
SearchResult SearchD760(const EdgeDesc &e, int x, int y) {
    if (x < 0 || x >= e.w || y < 0 || y >= e.h) return {x, y, 0};
    int yr = y + 1;
    if (yr < e.h - 1) {
        while (yr < e.h - 1) {
            if (!e.Present(x, yr, 0)) break;
            if (e.Present(x, yr, 1) || e.Present(x - 1, yr, 1)) break;
            ++yr;
        }
    }
    const int y_ret = yr - 1;
    if (y_ret >= e.h - 1) return {x, y_ret, 0};
    const int a1 = e.Present(x, y_ret + 1, 0) ? 1 : 0;
    const int a2 = (x >= 1 && e.Present(x - 1, y_ret + 1, 1)) ? 1 : 0;
    const int a3 = e.Present(x, y_ret + 1, 1) ? 1 : 0;
    const int a4 = (x >= 1 && e.Present(x - 1, y_ret + 1, 3)) ? 1 : 0;
    const int a5 = e.Present(x, y_ret + 1, 2) ? 1 : 0;
    return {x, y_ret, EndType(a1, a2, a3, a4, a5)};
}

// 0x18000D470: family C UP search on column x+1.
SearchResult SearchD470(const EdgeDesc &e, int x, int y) {
    const int cx = x + 1;
    if (cx < 0 || cx >= e.w || y < 0 || y >= e.h) return {cx, y, 0};
    int yr = y;
    if (y >= 1) {
        int count = y;
        for (;;) {
            if (!e.Present(cx, yr, 0)) { ++yr; break; }
            if (e.Present(cx, yr, 1) || e.Present(x, yr, 1)) break;
            --yr;
            if (--count < 1) break;
        }
    }
    const int a1 = (yr >= 1 && e.Present(cx, yr - 1, 0)) ? 1 : 0;
    const int a2 = e.Present(cx, yr, 1) ? 1 : 0;
    const int a3 = e.Present(x, yr, 1) ? 1 : 0;
    const int a4 = e.Present(x, yr, 3) ? 1 : 0;
    const int a5 = e.Present(cx, yr, 2) ? 1 : 0;
    return {x, yr, EndType(a1, a2, a3, a4, a5)};
}

// 0x18000DB10: family C DOWN search on column x+1.
SearchResult SearchDB10(const EdgeDesc &e, int x, int y) {
    const int cx = x + 1;
    if (cx < 0 || cx >= e.w || y < 0 || y >= e.h) return {cx, y, 0};
    int yr = y + 1;
    if (yr < e.h - 1) {
        while (yr < e.h - 1) {
            if (!e.Present(cx, yr, 0)) break;
            if (e.Present(cx, yr, 1) || e.Present(x, yr, 1)) break;
            ++yr;
        }
    }
    const int y_ret = yr - 1;
    if (y_ret >= e.h - 1) return {x, y_ret, 0};
    const int a1 = e.Present(cx, y_ret + 1, 0) ? 1 : 0;
    const int a2 = e.Present(x, y_ret + 1, 1) ? 1 : 0;
    const int a3 = e.Present(cx, y_ret + 1, 1) ? 1 : 0;
    const int a4 = e.Present(x, y_ret + 1, 3) ? 1 : 0;
    const int a5 = e.Present(cx, y_ret + 1, 2) ? 1 : 0;
    return {x, y_ret, EndType(a1, a2, a3, a4, a5)};
}

// 0x18000D5E0: family D LEFT search on row y.
SearchResult SearchD5E0(const EdgeDesc &e, int x, int y) {
    if (x < 0 || x >= e.w || y < 0 || y >= e.h) return {x, y, 0};
    int i = x;
    if (x >= 1) {
        int count = x;
        for (;;) {
            if (!e.Present(i, y, 1)) { ++i; break; }
            if (e.Present(i, y, 0) || e.Present(i, y - 1, 0)) break;
            --i;
            if (--count < 1) break;
        }
    }
    const int ir = i;
    const int a1 = (ir >= 1 && e.Present(ir - 1, y, 1)) ? 1 : 0;
    const int a2 = e.Present(ir, y - 1, 0) ? 1 : 0;
    const int a3 = e.Present(ir, y, 0) ? 1 : 0;
    const int a4 = e.Present(ir, y, 2) ? 1 : 0;
    const int a5 = (ir >= 1 && e.Present(ir - 1, y, 3)) ? 1 : 0;
    return {ir, y, EndType(a1, a2, a3, a4, a5)};
}

// 0x18000DC90: family D RIGHT search on row y.
SearchResult SearchDC90(const EdgeDesc &e, int x, int y) {
    if (x < 0 || x >= e.w || y < 0 || y >= e.h) return {x, y, 0};
    int i = x + 1;
    if (i < e.w - 1) {
        while (i < e.w - 1) {
            if (!e.Present(i, y, 1)) break;
            if (e.Present(i, y, 0) || e.Present(i, y - 1, 0)) break;
            ++i;
        }
    }
    const int ir = i - 1;
    // Binary reads the EndType bytes at the STOP pixel (i, y)/(i, y-1) — one
    // past the returned end — with a5 = b3(ir, y); OOB reads are 0.
    const int a1 = PresentIn(e, i, y, 1) ? 1 : 0;
    const int a2 = PresentIn(e, i, y, 0) ? 1 : 0;
    const int a3 = PresentIn(e, i, y - 1, 0) ? 1 : 0;
    const int a4 = PresentIn(e, i, y, 2) ? 1 : 0;
    const int a5 = PresentIn(e, ir, y, 3) ? 1 : 0;
    return {ir, y, EndType(a1, a2, a3, a4, a5)};
}

// --- Stair searches (0x180010C90/10A90/10990/10F00/10DE0/10B90/11010) --------
// Translated line-by-line from the decompiles; the walk/stop/restore
// semantics are preserved exactly. Each returns a 2-int position.

struct StairPos {
    int x;
    int y;
};

// 0x180010C90: up-left stair walk (for 11E40).
StairPos StairSearch10C90(const EdgeDesc &e, int x, int y) {
    const int sy = y + 1;
    if (sy >= e.h) return {x, y};
    const int limit = (sy < x) ? sy : x;
    if (limit == 0 || e.Present(x, sy, 2) || e.Present(x, y, 2)) return {x, y};
    int cx = x, cy = y;
    int walked = 0;
    while (true) {
        const int X = cx - 1;
        // (A) run-edge continuation; failure restores the pre-step position.
        if (!e.Present(X, cy, 1) || !e.Present(cx, cy, 0)) break;
        // (B) diagonal present at the old row -> stop at the full step.
        if (e.Present(X, cy, 2)) return {X, cy - 1};
        const int ny = cy - 1;
        // (C) diagonal present at the new row -> stop at the full step.
        const bool inb = (X >= 0 && X < e.w && ny >= 0 && ny < e.h);
        if (inb && e.Present(X, ny, 2)) return {X, ny};
        // (D) count exhausted -> stop at the full step.
        if (++walked >= limit) return {X, ny};
        cx = X;
        cy = ny;
    }
    return {cx, cy};
}

// 0x180010A90: down-right stair walk (for 11E40).
StairPos StairSearch10A90(const EdgeDesc &e, int x, int y) {
    const int sy = y + 1;
    if (sy >= e.h) return {x, y};
    // Binary: v9 = (h-sy <= w-x-1) ? h-sy : w-x; limit = v9 - 1.
    int v9 = e.w - x;
    if (e.h - sy <= v9 - 1) v9 = e.h - sy;
    const int limit = v9 - 1;
    if (limit == 0 || !e.Present(x + 1, sy, 0)) return {x, y};
    int cx = x, cy = y;
    int walked = 0;
    while (true) {
        const int X = cx + 1, Y = cy + 2;
        if (!e.Present(X, Y, 1)) break;
        if (e.Present(X, Y, 2) || e.Present(X, Y - 1, 2)) break;
        if (e.Present(X + 1, Y, 0)) {
            cx = X;
            cy = Y - 1;
            if (++walked >= limit) return {cx, cy};
            continue;
        }
        return {X, Y - 1};
    }
    return {cx, cy};
}

// 0x180010990: down-left stair walk (for 110F0).
StairPos StairSearch10990(const EdgeDesc &e, int x, int y) {
    const int sy = y + 1;
    if (sy >= e.h) return {x, y};
    int limit = e.h - sy - 1;
    if (limit >= x) limit = x;
    if (limit == 0 || !e.Present(x, sy, 0)) return {x, y};
    int cx = x, cy = y;
    int walked = 0;
    while (true) {
        const int X = cx - 1, Y = cy + 2;
        if (!e.Present(X, Y, 1)) break;
        if (e.Present(X, Y, 3) || e.Present(X, Y - 1, 3)) break;
        if (e.Present(X, Y, 0)) {
            cx = X;
            cy = Y - 1;
            if (++walked >= limit) return {cx, cy};
            continue;
        }
        return {X, Y - 1};
    }
    return {cx, cy};
}

// 0x180010F00: up-right stair walk (for 110F0).
StairPos StairSearch10F00(const EdgeDesc &e, int x, int y) {
    const int sy = y + 1;
    if (sy >= e.h) return {x, y};
    int limit = e.w - x - 1;
    if (sy < limit) limit = sy;
    if (limit == 0 || e.Present(x, sy, 3) || e.Present(x, y, 3)) return {x, y};
    int cx = x, cy = y;
    int walked = 0;
    while (true) {
        const int X = cx + 1, Y = cy;
        if (!e.Present(X, Y, 0) || !e.Present(X, Y, 1)) break;
        if (!e.Present(X, Y, 3)) {
            if (!e.Present(X, Y - 1, 3) && ++walked < limit) {
                cx = X;
                cy = Y - 1;
                continue;
            }
            return {X, Y - 1};
        }
        return {X, Y - 1};
    }
    return {cx, cy};
}

// 0x180010DE0: up-left stair walk (for 123A0). Binary state is (cur_x, cur_w)
// with cur_w starting at y-1; the candidate is (cur_x-1, cur_w). Break restores
// {cur_x, cur_w+1}; the b2-present / counter-exhausted paths return the full
// step {X, Y} (the earlier port double-decremented y and returned {X, Y-1}).
StairPos StairSearch10DE0(const EdgeDesc &e, int x, int y) {
    const int limit = (y < x) ? y : x;
    if (limit != 0 && !e.Present(x, y, 2) && !e.Present(x, y - 1, 2)) {
        int cx = x;
        int cw = y - 1;
        int walked = 0;
        for (;;) {
            const int X = cx - 1;
            const int Y = cw;
            if (!PresentIn(e, X, Y, 1) || !PresentIn(e, X + 1, Y, 0))
                return {cx, cw + 1};
            if (!PresentIn(e, X, Y, 2)) {
                if (!PresentIn(e, X, Y - 1, 2) && ++walked < limit) {
                    cx = X;
                    cw = Y - 1;
                    continue;
                }
            }
            return {X, Y};
        }
    }
    return {x, y};
}

// 0x180010B90: down-right stair walk (for 123A0).
StairPos StairSearch10B90(const EdgeDesc &e, int x, int y) {
    int limit = e.w - x;
    if (e.h - y <= limit - 1) limit = e.h - y;
    --limit;
    if (limit != 0 && e.Present(x + 1, y, 0)) {
        int cx = x, cy = y;
        int walked = 0;
        while (true) {
            const int X = cx + 1, Y = cy + 1;
            if (!e.Present(X, Y, 1)) break;
            if (e.Present(X, Y, 2) || e.Present(X, Y - 1, 2)) break;
            if (e.Present(X + 1, Y, 0)) {
                cx = X;
                cy = Y;
                if (++walked >= limit) return {cx, cy};
                continue;
            }
            return {X, Y};
        }
        return {cx, cy};
    }
    return {x, y};
}

// 0x180011010: up-right stair walk. In the binary this function is only
// referenced by the dirty-rect scan (0x18000B4C0), which the port skips;
// its loop body is inlined verbatim in StairHandler11660/11A60 below, so no
// standalone port exists (keeps the build warning-free).

// --- Re-classifiers (0x18000E1A0/DF60/E110/DED0/E230/DFF0/E2C0/E080) ---------
// args = {x0, y0, t0, x3, y4, t5}; each returns 4 at its border guard.

int ReclassE1A0(const EdgeDesc &e, const int *a) {  // A 1st (left end)
    const int S0 = a[0], S1 = a[1];
    if (S1 == 0) return 4;
    // Binary reads byte -3 = b1 of (S0-1, S1) (0x18000E1EF: [rcx+r8-3]).
    return (e.Present(S0, S1, 1) ? 1 : 0) + 2 * (e.Present(S0 - 1, S1, 1) ? 1 : 0) +
           4 * (e.Present(S0, S1 - 1, 0) ? 1 : 0);
}

int ReclassDF60(const EdgeDesc &e, const int *a) {  // A 2nd (right end)
    const int S3 = a[3], S4 = a[4];
    if (S4 == 0) return 4;
    return 2 * (e.Present(S3 + 1, S4, 1) ? 1 : 0) + (e.Present(S3, S4, 1) ? 1 : 0) +
           4 * (e.Present(S3 + 1, S4 - 1, 0) ? 1 : 0);
}

int ReclassE110(const EdgeDesc &e, const int *a) {  // B 1st (up end)
    const int S0 = a[0], S1 = a[1];
    if (S0 == e.w - 1) return 4;
    return (e.Present(S0 + 1, S1, 0) ? 1 : 0) + 2 * (e.Present(S0 + 1, S1 - 1, 0) ? 1 : 0) +
           4 * (e.Present(S0 + 1, S1, 1) ? 1 : 0);
}

int ReclassDED0(const EdgeDesc &e, const int *a) {  // B 2nd (down end)
    const int S3 = a[3], S4 = a[4];
    if (S3 == e.w - 1) return 4;
    // Binary 1-term is b0 of (S3+1, S4) (0x18000DF30: [r10+rcx], r10 = row S4,
    // col S3+1), not (S3, S4).
    return 2 * (e.Present(S3 + 1, S4 + 1, 0) ? 1 : 0) +
           4 * (e.Present(S3 + 1, S4 + 1, 1) ? 1 : 0) +
           (e.Present(S3 + 1, S4, 0) ? 1 : 0);
}

int ReclassE230(const EdgeDesc &e, const int *a) {  // C 1st (up end)
    const int S0 = a[0], S1 = a[1];
    if (S0 == 0) return 4;
    // Binary 4-term reads byte -3 = b1 of (S0-1, S1) (0x18000E282).
    return (e.Present(S0, S1, 0) ? 1 : 0) + 2 * (e.Present(S0, S1 - 1, 0) ? 1 : 0) +
           4 * (e.Present(S0 - 1, S1, 1) ? 1 : 0);
}

int ReclassDFF0(const EdgeDesc &e, const int *a) {  // C 2nd (down end)
    const int S3 = a[3], S4 = a[4];
    if (S3 == 0) return 4;
    // Binary 4-term reads byte -3 = b1 of (S3-1, S4+1) (0x18000E04D).
    return 2 * (e.Present(S3, S4 + 1, 0) ? 1 : 0) + (e.Present(S3, S4, 0) ? 1 : 0) +
           4 * (e.Present(S3 - 1, S4 + 1, 1) ? 1 : 0);
}

int ReclassE2C0(const EdgeDesc &e, const int *a) {  // D 1st (left end)
    const int S0 = a[0], S1 = a[1];
    if (S1 == e.h - 1) return 4;
    // Binary 2-term reads byte -3 = b1 of (S0-1, S1+1) (0x18000E318).
    return 4 * (e.Present(S0, S1 + 1, 0) ? 1 : 0) + (e.Present(S0, S1 + 1, 1) ? 1 : 0) +
           2 * (e.Present(S0 - 1, S1 + 1, 1) ? 1 : 0);
}

int ReclassE080(const EdgeDesc &e, const int *a) {  // D 2nd (right end)
    const int S3 = a[3], S4 = a[4];
    if (S4 == e.h - 1) return 4;
    return 4 * (e.Present(S3 + 1, S4 + 1, 0) ? 1 : 0) +
           2 * (e.Present(S3 + 1, S4 + 1, 1) ? 1 : 0) +
           (e.Present(S3, S4 + 1, 1) ? 1 : 0);
}

// --- Appenders (0x18000E4F0/E3E0/E460/E350) ----------------------------------
// args = {x0, y0, t0, x3, y4, t5}; L = run * scale * s; sample just outside
// the end; w == 0 appends nothing and returns false.

bool AppendE4F0(MlaaCtx &c, const int *a, float scale, float h_in) {  // A/D 1st (left)
    const float h = h_in * 0.5f;
    const float L = (float)(a[3] - a[0] + 1) * scale * c.s;
    const float w = Ramp(L, c.x - a[0], h);
    if (w == 0.0f) return false;
    AppendSample(c, a[0] - 1, a[1], w);
    return true;
}

bool AppendE3E0(MlaaCtx &c, const int *a, float scale, float h_in) {  // A/D 2nd (right)
    const float h = h_in * 0.5f;
    const float L = (float)(a[3] - a[0] + 1) * scale * c.s;
    const float w = Ramp(L, a[3] - c.x, h);
    if (w == 0.0f) return false;
    AppendSample(c, a[3] + 1, a[4], w);
    return true;
}

bool AppendE460(MlaaCtx &c, const int *a, float scale, float h_in) {  // B/C 1st (up)
    const float h = h_in * 0.5f;
    const float L = (float)(a[4] - a[1] + 1) * scale * c.s;
    const float w = Ramp(L, c.y - a[1], h);
    if (w == 0.0f) return false;
    AppendSample(c, a[0], a[1] - 1, w);
    return true;
}

bool AppendE350(MlaaCtx &c, const int *a, float scale, float h_in) {  // B/C 2nd (down)
    const float h = h_in * 0.5f;
    const float L = (float)(a[4] - a[1] + 1) * scale * c.s;
    const float w = Ramp(L, a[4] - c.y, h);
    if (w == 0.0f) return false;
    AppendSample(c, a[3], a[4] + 1, w);
    return true;
}

// --- Extended ends (0x18000ED00/E700/EB90/EE70/EFE0/E570/E880/EA10) ----------
// Gate t in {1,3,7}; scale from a fresh search at the end; walk the far
// direction while type 4 (A/D/B up) or 3 (C/E*), h_in 1.0 on the early
// type-hit, else 0.5.

bool ExtendedA1st(MlaaCtx &c, const EdgeDesc &e, const int *a) {  // 0x18000ED00
    const int t = ReclassE1A0(e, a);
    if (!(((t - 1) & 0xFFFFFFF9) == 0) || t == 5) return false;
    const SearchResult walked = SearchDC90(e, a[0], a[1]);
    // Binary numerator (0x18000ED75-ED9A): walked.x - a[0] + 1.
    const float scale = (float)(walked.x - a[0] + 1) * (c.es * 0.2f + 0.5f) /
                        (float)(a[3] - a[0] + 1);
    float h_in = 1.0f;
    if (t == 3 || t == 7) {
        h_in = 0.5f;
        int px = a[0] - 1, py = a[1];
        for (;;) {
            if (px < 0 || px >= e.w || py < 0 || py >= e.h) break;
            const SearchResult r = SearchD2F0(e, px, py);
            if (r.type == 1) { h_in = 1.0f; break; }
            if (r.type != 4) break;
            px = r.x - 1;
            py = r.y;
        }
    }
    return AppendE4F0(c, a, scale, h_in);
}

bool ExtendedA2nd(MlaaCtx &c, const EdgeDesc &e, const int *a) {  // 0x18000E700
    const int t = ReclassDF60(e, a);
    if (!(((t - 1) & 0xFFFFFFF9) == 0) || t == 5) return false;
    const SearchResult walked = SearchD5E0(e, a[3], a[4]);
    const float scale = (float)(a[3] - walked.x + 1) * (c.es * 0.2f + 0.5f) /
                        (float)(a[3] - a[0] + 1);
    float h_in = 1.0f;
    if (t == 3 || t == 7) {
        h_in = 0.5f;
        int px = a[3] + 1, py = a[4];
        for (;;) {
            if (px < 0 || px >= e.w || py < 0 || py >= e.h) break;
            const SearchResult r = SearchD8C0(e, px, py);
            if (r.type == 2) { h_in = 1.0f; break; }
            if (r.type != 3) break;
            px = r.x + 1;
            py = r.y;
        }
    }
    return AppendE3E0(c, a, scale, h_in);
}

bool ExtendedB1st(MlaaCtx &c, const EdgeDesc &e, const int *a) {  // 0x18000EB90
    const int t = ReclassE110(e, a);
    if (!(((t - 1) & 0xFFFFFFF9) == 0) || t == 5) return false;
    const SearchResult walked = SearchDB10(e, a[0], a[1]);
    const float scale = (float)(walked.y - a[1] + 1) * (c.es * 0.2f + 0.5f) /
                        (float)(a[4] - a[1] + 1);
    float h_in = 1.0f;
    if (t == 3 || t == 7) {
        h_in = 0.5f;
        int px = a[0], py = a[1] - 1;
        for (;;) {
            if (px < 0 || px >= e.w || py < 0 || py >= e.h) break;
            const SearchResult r = SearchCFA0(e, px, py);
            if (r.type == 1) { h_in = 1.0f; break; }
            if (r.type != 4) break;
            px = r.x;
            py = r.y - 1;
        }
    }
    return AppendE460(c, a, scale, h_in);
}

bool ExtendedC1st(MlaaCtx &c, const EdgeDesc &e, const int *a) {  // 0x18000EE70
    const int t = ReclassE230(e, a);
    if (!(((t - 1) & 0xFFFFFFF9) == 0) || t == 5) return false;
    const SearchResult walked = SearchD760(e, a[0], a[1]);
    const float scale = (float)(walked.y - a[1] + 1) * (c.es * 0.2f + 0.5f) /
                        (float)(a[4] - a[1] + 1);
    float h_in = 1.0f;
    if (t == 3 || t == 7) {
        h_in = 0.5f;
        int px = a[0], py = a[1] - 1;
        for (;;) {
            if (px < 0 || px >= e.w || py < 0 || py >= e.h) break;
            const SearchResult r = SearchD470(e, px, py);
            if (r.type == 2) { h_in = 1.0f; break; }
            if (r.type != 3) break;
            px = r.x;
            py = r.y - 1;
        }
    }
    return AppendE460(c, a, scale, h_in);
}

bool ExtendedD1st(MlaaCtx &c, const EdgeDesc &e, const int *a) {  // 0x18000EFE0
    const int t = ReclassE2C0(e, a);
    if (!(((t - 1) & 0xFFFFFFF9) == 0) || t == 5) return false;
    const SearchResult walked = SearchD8C0(e, a[0], a[1]);
    const float scale = (float)(walked.x - a[0] + 1) * (c.es * 0.2f + 0.5f) /
                        (float)(a[3] - a[0] + 1);
    float h_in = 1.0f;
    if (t == 3 || t == 7) {
        h_in = 0.5f;
        int px = a[0] - 1, py = a[1];
        for (;;) {
            if (px < 0 || px >= e.w || py < 0 || py >= e.h) break;
            const SearchResult r = SearchD5E0(e, px, py);
            if (r.type == 2) { h_in = 1.0f; break; }
            if (r.type != 3) break;
            px = r.x - 1;
            py = r.y;
        }
    }
    return AppendE4F0(c, a, scale, h_in);
}

bool ExtendedB2nd(MlaaCtx &c, const EdgeDesc &e, const int *a) {  // 0x18000E570
    const int t = ReclassDED0(e, a);
    if (!(((t - 1) & 0xFFFFFFF9) == 0) || t == 5) return false;
    const SearchResult walked = SearchD470(e, a[3], a[4]);
    const float scale = (float)(a[4] - walked.y + 1) * (c.es * 0.2f + 0.5f) /
                        (float)(a[4] - a[1] + 1);
    float h_in = 1.0f;
    if (t == 3 || t == 7) {
        h_in = 0.5f;
        int px = a[3], py = a[4] + 1;
        for (;;) {
            if (px < 0 || px >= e.w || py < 0 || py >= e.h) break;
            const SearchResult r = SearchD760(e, px, py);
            if (r.type == 2) { h_in = 1.0f; break; }
            if (r.type != 3) break;
            px = r.x;
            py = r.y + 1;
        }
    }
    return AppendE350(c, a, scale, h_in);
}

bool ExtendedC2nd(MlaaCtx &c, const EdgeDesc &e, const int *a) {  // 0x18000E880
    const int t = ReclassDFF0(e, a);
    if (!(((t - 1) & 0xFFFFFFF9) == 0) || t == 5) return false;
    const SearchResult walked = SearchCFA0(e, a[3], a[4]);
    const float scale = (float)(a[4] - walked.y + 1) * (c.es * 0.2f + 0.5f) /
                        (float)(a[4] - a[1] + 1);
    float h_in = 1.0f;
    if (t == 3 || t == 7) {
        h_in = 0.5f;
        int px = a[3], py = a[4] + 1;
        for (;;) {
            if (px < 0 || px >= e.w || py < 0 || py >= e.h) break;
            const SearchResult r = SearchDB10(e, px, py);
            if (r.type == 1) { h_in = 1.0f; break; }
            if (r.type != 4) break;
            px = r.x;
            py = r.y + 1;
        }
    }
    return AppendE350(c, a, scale, h_in);
}

bool ExtendedD2nd(MlaaCtx &c, const EdgeDesc &e, const int *a) {  // 0x18000EA10
    const int t = ReclassE080(e, a);
    if (!(((t - 1) & 0xFFFFFFF9) == 0) || t == 5) return false;
    const SearchResult walked = SearchD2F0(e, a[3], a[4]);
    const float scale = (float)(a[3] - walked.x + 1) * (c.es * 0.2f + 0.5f) /
                        (float)(a[3] - a[0] + 1);
    float h_in = 1.0f;
    if (t == 3 || t == 7) {
        h_in = 0.5f;
        int px = a[3] + 1, py = a[4];
        for (;;) {
            if (px < 0 || px >= e.w || py < 0 || py >= e.h) break;
            const SearchResult r = SearchDC90(e, px, py);
            if (r.type == 1) { h_in = 1.0f; break; }
            if (r.type != 4) break;
            px = r.x + 1;
            py = r.y;
        }
    }
    return AppendE3E0(c, a, scale, h_in);
}

// --- Family appender dispatch (0x18000FCB0/F9B0/FFB0/102A0) ------------------

void DoubleLastWeight(MlaaCtx &c) {
    // (w + w) * (w + w) * 0.5 on the last appended record.
    if (c.count == 0) return;
    float &w = c.samples[c.count - 1][4];
    w = (w + w) * (w + w) * 0.5f;
}

// --- Binary helper layer ------------------------------------------------------
// The binary funnels every family-dispatcher case through small gate helpers:
//   Plain (F560/F420/F2E0/F1A0/F510/F3D0/F290/F150/F5B0/F330/F1F0/F600/F380/
//          F240/F4C0/F470): reclass != 4 -> append(scale, h = 1.0)
//   Split (F870/F6C0/F800/F650/F8E0/F730/F950/F7A0): reclass in {2,3,6,7} ->
//          append(fifth scale, h = 1.0 for t in {2,6} else 0.5)
// scale = es*0.5+0.5 (half) or es*0.2+0.5 (fifth); the appenders halve h.

enum ReclassId { kE1A0, kDF60, kE110, kDED0, kE230, kDFF0, kE2C0, kE080 };
enum AppendId { kE4F0, kE3E0, kE460, kE350 };

int Reclass(ReclassId id, const EdgeDesc &e, const int *a) {
    switch (id) {
        case kE1A0: return ReclassE1A0(e, a);
        case kDF60: return ReclassDF60(e, a);
        case kE110: return ReclassE110(e, a);
        case kDED0: return ReclassDED0(e, a);
        case kE230: return ReclassE230(e, a);
        case kDFF0: return ReclassDFF0(e, a);
        case kE2C0: return ReclassE2C0(e, a);
        default: return ReclassE080(e, a);
    }
}

bool AppendVia(MlaaCtx &c, const int *a, AppendId id, float scale, float h) {
    switch (id) {
        case kE4F0: return AppendE4F0(c, a, scale, h);
        case kE3E0: return AppendE3E0(c, a, scale, h);
        case kE460: return AppendE460(c, a, scale, h);
        default: return AppendE350(c, a, scale, h);
    }
}

bool AppendPlain(MlaaCtx &c, const EdgeDesc &e, const int *a, ReclassId r, AppendId ap,
                 bool fifth) {
    if (Reclass(r, e, a) == 4) return false;
    const float scale = fifth ? c.es * 0.2f + 0.5f : c.es * 0.5f + 0.5f;
    return AppendVia(c, a, ap, scale, 1.0f);
}

bool AppendSplit(MlaaCtx &c, const EdgeDesc &e, const int *a, ReclassId r, AppendId ap) {
    const int t = Reclass(r, e, a);
    if (t != 2 && t != 3 && t != 6 && t != 7) return false;
    const float h = (t == 2 || t == 6) ? 1.0f : 0.5f;
    return AppendVia(c, a, ap, c.es * 0.2f + 0.5f, h);
}

// Family A (0x18000FCB0): selector ltype + 10*rtype - 2.
void FamilyAAppend(MlaaCtx &c, const EdgeDesc &e, const int *a) {
    const int sel = a[2] + 10 * a[5] - 2;
    switch (sel) {
        case 0: case 6: case 20: case 26: case 30: case 36: case 80: case 86:
            AppendPlain(c, e, a, kE1A0, kE4F0, false);  // F560
            return;
        case 1: case 7: case 27: case 31: case 37: case 51: case 57: case 87:
            ExtendedA1st(c, e, a);  // ED00
            return;
        case 4: case 24: case 34: case 54: case 84:
            AppendSplit(c, e, a, kE1A0, kE4F0);  // F870
            return;
        case 8: case 9: case 12: case 15: case 68: case 69: case 72: case 75:
            AppendPlain(c, e, a, kDF60, kE3E0, false);  // F420
            return;
        case 10: case 16: case 70: case 76:
            if (AppendPlain(c, e, a, kE1A0, kE4F0, true)) DoubleLastWeight(c);   // F2E0
            if (AppendPlain(c, e, a, kDF60, kE3E0, true)) DoubleLastWeight(c);   // F1A0
            return;
        case 11: case 71:
            ExtendedA1st(c, e, a);
            AppendPlain(c, e, a, kDF60, kE3E0, true);  // F1A0
            return;
        case 13: case 73:
            AppendPlain(c, e, a, kDF60, kE3E0, true);  // F1A0
            return;
        case 14: case 74:
            AppendSplit(c, e, a, kE1A0, kE4F0);        // F870
            AppendPlain(c, e, a, kDF60, kE3E0, true);  // F1A0
            return;
        case 17: case 77:
            ExtendedA1st(c, e, a);
            AppendPlain(c, e, a, kDF60, kE3E0, false);  // F420
            return;
        case 38: case 42: case 43: case 88: case 89: case 92: case 93: case 95:
            ExtendedA2nd(c, e, a);  // E700
            return;
        case 40: case 46:
            AppendPlain(c, e, a, kE1A0, kE4F0, true);  // F2E0
            ExtendedA2nd(c, e, a);
            return;
        case 41: case 47: case 91: case 97:
            ExtendedA1st(c, e, a);
            ExtendedA2nd(c, e, a);
            return;
        case 44: case 94:
            AppendSplit(c, e, a, kE1A0, kE4F0);  // F870
            ExtendedA2nd(c, e, a);
            return;
        case 50: case 56:
            AppendPlain(c, e, a, kE1A0, kE4F0, true);  // F2E0
            return;
        case 58: case 59: case 62: case 63: case 65:
            AppendSplit(c, e, a, kDF60, kE3E0);  // F6C0
            return;
        case 60: case 66:
            AppendPlain(c, e, a, kE1A0, kE4F0, true);  // F2E0
            AppendSplit(c, e, a, kDF60, kE3E0);        // F6C0
            return;
        case 61: case 67:
            ExtendedA1st(c, e, a);
            AppendSplit(c, e, a, kDF60, kE3E0);  // F6C0
            return;
        case 64:
            AppendSplit(c, e, a, kE1A0, kE4F0);  // F870
            AppendSplit(c, e, a, kDF60, kE3E0);  // F6C0
            return;
        case 90: case 96:
            AppendPlain(c, e, a, kE1A0, kE4F0, false);  // F560
            ExtendedA2nd(c, e, a);
            return;
        default:
            return;
    }
}

// Family B (0x18000F9B0): selector ltype + 10*rtype - 2.
void FamilyBAppend(MlaaCtx &c, const EdgeDesc &e, const int *a) {
    const int sel = a[2] + 10 * a[5] - 2;
    switch (sel) {
        case 0: case 6: case 20: case 26: case 30: case 36: case 80: case 86:
            AppendPlain(c, e, a, kE110, kE460, false);  // F510
            return;
        case 1: case 7: case 27: case 31: case 37: case 51: case 57: case 87:
            ExtendedB1st(c, e, a);  // EB90
            return;
        case 4: case 24: case 34: case 54: case 84:
            AppendSplit(c, e, a, kE110, kE460);  // F800
            return;
        case 8: case 9: case 12: case 15: case 68: case 69: case 72: case 75:
            AppendPlain(c, e, a, kDED0, kE350, false);  // F3D0
            return;
        case 10: case 16: case 70: case 76:
            if (AppendPlain(c, e, a, kE110, kE460, true)) DoubleLastWeight(c);  // F290
            if (AppendPlain(c, e, a, kDED0, kE350, true)) DoubleLastWeight(c);  // F150
            return;
        case 11: case 71:
            ExtendedB1st(c, e, a);
            AppendPlain(c, e, a, kDED0, kE350, true);  // F150
            return;
        case 13: case 73:
            AppendPlain(c, e, a, kDED0, kE350, true);  // F150
            return;
        case 14: case 74:
            AppendSplit(c, e, a, kE110, kE460);        // F800
            AppendPlain(c, e, a, kDED0, kE350, true);  // F150
            return;
        case 17: case 77:
            ExtendedB1st(c, e, a);
            AppendPlain(c, e, a, kDED0, kE350, false);  // F3D0
            return;
        case 38: case 42: case 43: case 88: case 89: case 92: case 93: case 95:
            ExtendedB2nd(c, e, a);  // E570
            return;
        case 40: case 46:
            AppendPlain(c, e, a, kE110, kE460, true);  // F290
            ExtendedB2nd(c, e, a);
            return;
        case 41: case 47: case 91: case 97:
            ExtendedB1st(c, e, a);
            ExtendedB2nd(c, e, a);
            return;
        case 44: case 94:
            AppendSplit(c, e, a, kE110, kE460);  // F800
            ExtendedB2nd(c, e, a);
            return;
        case 50: case 56:
            AppendPlain(c, e, a, kE110, kE460, true);  // F290
            return;
        case 58: case 59: case 62: case 63: case 65:
            AppendSplit(c, e, a, kDED0, kE350);  // F650
            return;
        case 60: case 66:
            AppendPlain(c, e, a, kE110, kE460, true);  // F290
            AppendSplit(c, e, a, kDED0, kE350);        // F650
            return;
        case 61: case 67:
            ExtendedB1st(c, e, a);
            AppendSplit(c, e, a, kDED0, kE350);  // F650
            return;
        case 64:
            AppendSplit(c, e, a, kE110, kE460);  // F800
            AppendSplit(c, e, a, kDED0, kE350);  // F650
            return;
        case 90: case 96:
            AppendPlain(c, e, a, kE110, kE460, false);  // F510
            ExtendedB2nd(c, e, a);
            return;
        default:
            return;
    }
}

// Family C (0x18000FFB0): selector ltype + 10*rtype - 1.
// NOTE: the 2nd end is DFF0-gated and appends via E350 (down), not E080/E3E0
// (that is family D's table) — helpers F470 (half) / F1F0 (fifth) / F730 (split).
void FamilyCAppend(MlaaCtx &c, const EdgeDesc &e, const int *a) {
    const int sel = a[2] - 1 + 10 * a[5];
    switch (sel) {
        case 0: case 6: case 10: case 16: case 40: case 46: case 70: case 76:
            AppendPlain(c, e, a, kE230, kE460, false);  // F5B0
            return;
        case 3: case 8: case 18: case 43: case 48: case 53: case 58: case 78:
            ExtendedC1st(c, e, a);  // EE70
            return;
        case 5: case 15: case 45: case 55: case 75:
            AppendSplit(c, e, a, kE230, kE460);  // F8E0
            return;
        case 19: case 21: case 22: case 27: case 79: case 81: case 82: case 87:
            AppendPlain(c, e, a, kDFF0, kE350, false);  // F470
            return;
        case 20: case 26: case 80: case 86:
            if (AppendPlain(c, e, a, kE230, kE460, true)) DoubleLastWeight(c);  // F330
            if (AppendPlain(c, e, a, kDFF0, kE350, true)) DoubleLastWeight(c);  // F1F0
            return;
        case 23: case 83:
            ExtendedC1st(c, e, a);
            AppendPlain(c, e, a, kDFF0, kE350, true);  // F1F0
            return;
        case 24: case 84:
            AppendPlain(c, e, a, kDFF0, kE350, true);  // F1F0
            return;
        case 25: case 85:
            AppendSplit(c, e, a, kE230, kE460);        // F8E0
            AppendPlain(c, e, a, kDFF0, kE350, true);  // F1F0
            return;
        case 28: case 88:
            ExtendedC1st(c, e, a);
            AppendPlain(c, e, a, kDFF0, kE350, false);  // F470
            return;
        case 29: case 32: case 34: case 89: case 91: case 92: case 94: case 97:
            ExtendedC2nd(c, e, a);  // E880
            return;
        case 30: case 36:
            AppendPlain(c, e, a, kE230, kE460, true);  // F330
            ExtendedC2nd(c, e, a);
            return;
        case 33: case 38: case 93: case 98:
            ExtendedC1st(c, e, a);
            ExtendedC2nd(c, e, a);
            return;
        case 35: case 95:
            AppendSplit(c, e, a, kE230, kE460);  // F8E0
            ExtendedC2nd(c, e, a);
            return;
        case 50: case 56:
            AppendPlain(c, e, a, kE230, kE460, true);  // F330
            return;
        case 59: case 61: case 62: case 64: case 67:
            AppendSplit(c, e, a, kDFF0, kE350);  // F730
            return;
        case 60: case 66:
            AppendPlain(c, e, a, kE230, kE460, true);  // F330
            AppendSplit(c, e, a, kDFF0, kE350);        // F730
            return;
        case 63: case 68:
            ExtendedC1st(c, e, a);
            AppendSplit(c, e, a, kDFF0, kE350);  // F730
            return;
        case 65:
            AppendSplit(c, e, a, kE230, kE460);  // F8E0
            AppendSplit(c, e, a, kDFF0, kE350);  // F730
            return;
        case 90: case 96:
            AppendPlain(c, e, a, kE230, kE460, false);  // F5B0
            ExtendedC2nd(c, e, a);
            return;
        default:
            return;
    }
}

// Family D (0x1800102A0): selector ltype + 10*rtype - 1.
void FamilyDAppend(MlaaCtx &c, const EdgeDesc &e, const int *a) {
    const int sel = a[2] - 1 + 10 * a[5];
    switch (sel) {
        case 0: case 6: case 10: case 16: case 40: case 46: case 70: case 76:
            AppendPlain(c, e, a, kE2C0, kE4F0, false);  // F600
            return;
        case 3: case 8: case 18: case 43: case 48: case 53: case 58: case 78:
            ExtendedD1st(c, e, a);  // EFE0
            return;
        case 5: case 15: case 45: case 55: case 75:
            AppendSplit(c, e, a, kE2C0, kE4F0);  // F950
            return;
        case 19: case 21: case 22: case 27: case 79: case 81: case 82: case 87:
            AppendPlain(c, e, a, kE080, kE3E0, false);  // F4C0
            return;
        case 20: case 26: case 80: case 86:
            if (AppendPlain(c, e, a, kE2C0, kE4F0, true)) DoubleLastWeight(c);  // F380
            if (AppendPlain(c, e, a, kE080, kE3E0, true)) DoubleLastWeight(c);  // F240
            return;
        case 23: case 83:
            ExtendedD1st(c, e, a);
            AppendPlain(c, e, a, kE080, kE3E0, true);  // F240
            return;
        case 24: case 84:
            AppendPlain(c, e, a, kE080, kE3E0, true);  // F240
            return;
        case 25: case 85:
            AppendSplit(c, e, a, kE2C0, kE4F0);        // F950
            AppendPlain(c, e, a, kE080, kE3E0, true);  // F240
            return;
        case 28: case 88:
            ExtendedD1st(c, e, a);
            AppendPlain(c, e, a, kE080, kE3E0, false);  // F4C0
            return;
        case 29: case 32: case 34: case 89: case 91: case 92: case 94: case 97:
            ExtendedD2nd(c, e, a);  // EA10
            return;
        case 30: case 36:
            AppendPlain(c, e, a, kE2C0, kE4F0, true);  // F380
            ExtendedD2nd(c, e, a);
            return;
        case 33: case 38: case 93: case 98:
            ExtendedD1st(c, e, a);
            ExtendedD2nd(c, e, a);
            return;
        case 35: case 95:
            AppendSplit(c, e, a, kE2C0, kE4F0);  // F950
            ExtendedD2nd(c, e, a);
            return;
        case 50: case 56:
            AppendPlain(c, e, a, kE2C0, kE4F0, true);  // F380
            return;
        case 59: case 61: case 62: case 64: case 67:
            AppendSplit(c, e, a, kE080, kE3E0);  // F7A0
            return;
        case 60: case 66:
            AppendPlain(c, e, a, kE2C0, kE4F0, true);  // F380
            AppendSplit(c, e, a, kE080, kE3E0);        // F7A0
            return;
        case 63: case 68:
            ExtendedD1st(c, e, a);
            AppendSplit(c, e, a, kE080, kE3E0);  // F7A0
            return;
        case 65:
            AppendSplit(c, e, a, kE2C0, kE4F0);  // F950
            AppendSplit(c, e, a, kE080, kE3E0);  // F7A0
            return;
        case 90: case 96:
            AppendPlain(c, e, a, kE2C0, kE4F0, false);  // F600
            ExtendedD2nd(c, e, a);
            return;
        default:
            return;
    }
}

// --- Family mains (0x1800106B0/10770/10820/108E0) ----------------------------

void FamilyAMain(MlaaCtx &c) {  // edge BELOW (y != H-1)
    const EdgeDesc &e = *c.edge;
    if (c.y == e.h - 1) return;
    const SearchResult l = SearchD2F0(e, c.x, c.y);
    const SearchResult r = SearchD8C0(e, c.x, c.y);
    const int a[6] = {l.x, l.y, l.type, r.x, r.y, r.type};
    FamilyAAppend(c, e, a);
}

void FamilyBMain(MlaaCtx &c) {  // edge LEFT (x != 0)
    const EdgeDesc &e = *c.edge;
    if (c.x == 0) return;
    const SearchResult u = SearchCFA0(e, c.x, c.y);
    const SearchResult d = SearchD760(e, c.x, c.y);
    const int a[6] = {u.x, u.y, u.type, d.x, d.y, d.type};
    FamilyBAppend(c, e, a);
}

void FamilyCMain(MlaaCtx &c) {  // edge RIGHT (x != W-1)
    const EdgeDesc &e = *c.edge;
    if (c.x == e.w - 1) return;
    const SearchResult u = SearchD470(e, c.x, c.y);
    const SearchResult d = SearchDB10(e, c.x, c.y);
    const int a[6] = {u.x, u.y, u.type, d.x, d.y, d.type};
    FamilyCAppend(c, e, a);
}

void FamilyDMain(MlaaCtx &c) {  // edge ABOVE (y != 0)
    const EdgeDesc &e = *c.edge;
    if (c.y == 0) return;
    const SearchResult l = SearchD5E0(e, c.x, c.y);
    const SearchResult r = SearchDC90(e, c.x, c.y);
    const int a[6] = {l.x, l.y, l.type, r.x, r.y, r.type};
    FamilyDAppend(c, e, a);
}

// --- Corners (0x180012CE0/12DA0/13580/13630) ---------------------------------
// k = base * s (base 0.125 from the dispatcher); samples at 0.4k / 0.2k / 0.4k.

void CornerDownLeft(MlaaCtx &c, float base) {   // 0x180012CE0
    if (c.x == 0 || c.y == c.canvas->h - 1) return;
    const float k = base * c.s;
    AppendSample(c, c.x - 1, c.y, k * 0.40000001f);
    AppendSample(c, c.x - 1, c.y + 1, k * 0.2f);
    AppendSample(c, c.x, c.y + 1, k * 0.40000001f);
}

void CornerDownRight(MlaaCtx &c, float base) {  // 0x180012DA0
    if (c.x == c.canvas->w - 1 || c.y == c.canvas->h - 1) return;
    const float k = base * c.s;
    AppendSample(c, c.x, c.y + 1, k * 0.40000001f);
    AppendSample(c, c.x + 1, c.y + 1, k * 0.2f);
    AppendSample(c, c.x + 1, c.y, k * 0.40000001f);
}

void CornerUpLeft(MlaaCtx &c, float base) {     // 0x180013580
    if (c.x == 0 || c.y == 0) return;
    const float k = base * c.s;
    AppendSample(c, c.x - 1, c.y, k * 0.40000001f);
    AppendSample(c, c.x - 1, c.y - 1, k * 0.2f);
    AppendSample(c, c.x, c.y - 1, k * 0.40000001f);
}

void CornerUpRight(MlaaCtx &c, float base) {    // 0x180013630
    if (c.x == c.canvas->w - 1 || c.y == 0) return;
    const float k = base * c.s;
    AppendSample(c, c.x, c.y - 1, k * 0.40000001f);
    AppendSample(c, c.x + 1, c.y - 1, k * 0.2f);
    AppendSample(c, c.x + 1, c.y, k * 0.40000001f);
}

// --- Run-gated corners (0x180012E60/13020/133B0/13200) -----------------------
// base = 0.125 when min(run1, run2) == 3 else 0.5 (12E60/13200 compare the
// raw runs; 13020/133B0 compare run-1, same predicate); a diagonal presence
// byte gates everything. The dispatcher halves s for cases 10/138 (12E60)
// and 72/76 (133B0) via the s_scale argument; 13020/13200 run at full s.

void RunGatedDownLeft(MlaaCtx &c, float s_scale) {  // 0x180012E60
    const EdgeDesc &e = *c.edge;
    if (c.y == e.h - 1 || c.x == 0) return;
    const SearchResult r1 = SearchD8C0(e, c.x, c.y);
    const int run1 = r1.x - c.x + 1;
    const SearchResult r2 = SearchCFA0(e, c.x, c.y);  // D190 is a CFA0 twin
    const int run2 = c.y - r2.y + 1;
    if (run1 < 4 || run2 < 4) {
        // Binary 0x180012E60 appends when run1<2 || run2<2 || b2(x,y+1) ABSENT
        // (the earlier port had this gate inverted, skipping every such corner).
        if (run1 >= 2 && run2 >= 2 && e.Present(c.x, c.y + 1, 2)) return;
        const int mn = run1 < run2 ? run1 : run2;
        const float base = (mn == 3) ? 0.125f : 0.5f;
        const float k = base * (c.s * s_scale);
        AppendSample(c, c.x - 1, c.y, k * 0.40000001f);
        AppendSample(c, c.x - 1, c.y + 1, k * 0.2f);
        AppendSample(c, c.x, c.y + 1, k * 0.40000001f);
    }
}

void RunGatedDownRight(MlaaCtx &c, float s_scale) {  // 0x180013020
    const EdgeDesc &e = *c.edge;
    if (c.y == e.h - 1 || c.x == e.w - 1) return;
    const SearchResult r1 = SearchD2F0(e, c.x, c.y);
    const int run1 = c.x - r1.x + 1;
    const SearchResult r2 = SearchD470(e, c.x, c.y);
    const int run2 = c.y - r2.y + 1;
    if (run1 < 4 || run2 < 4) {
        // Binary 0x180013020 appends when run1<2 || run2<2 || b3(x,y+1) ABSENT.
        if (run1 >= 2 && run2 >= 2 && e.Present(c.x, c.y + 1, 3)) return;
        const int m1 = run1 - 1, m2 = run2 - 1;
        const int mn = m1 < m2 ? m1 : m2;
        const float base = (mn == 2) ? 0.125f : 0.5f;
        const float k = base * (c.s * s_scale);
        AppendSample(c, c.x, c.y + 1, k * 0.40000001f);
        AppendSample(c, c.x + 1, c.y + 1, k * 0.2f);
        AppendSample(c, c.x + 1, c.y, k * 0.40000001f);
    }
}

void RunGatedUpLeft(MlaaCtx &c, float s_scale) {  // 0x1800133B0
    const EdgeDesc &e = *c.edge;
    if (c.y == 0 || c.x == e.w - 1) return;
    const SearchResult r1 = SearchD5E0(e, c.x, c.y);
    const int run1 = c.x - r1.x + 1;
    const SearchResult r2 = SearchDB10(e, c.x, c.y);
    const int run2 = r2.y - c.y + 1;
    if (run1 < 4 || run2 < 4) {
        // Binary 0x1800133B0 appends when run1<2 || run2<2 || b2(x+1,y) ABSENT.
        if (run1 >= 2 && run2 >= 2 && e.Present(c.x + 1, c.y, 2)) return;
        const int m1 = run1 - 1, m2 = run2 - 1;
        const int mn = m1 < m2 ? m1 : m2;
        const float base = (mn == 2) ? 0.125f : 0.5f;
        const float k = base * (c.s * s_scale);
        AppendSample(c, c.x, c.y - 1, k * 0.40000001f);
        AppendSample(c, c.x + 1, c.y - 1, k * 0.2f);
        AppendSample(c, c.x + 1, c.y, k * 0.40000001f);
    }
}

void RunGatedUpRight(MlaaCtx &c, float s_scale) {  // 0x180013200
    const EdgeDesc &e = *c.edge;
    if (c.y == e.h - 1 || c.x == 0) return;
    const SearchResult r1 = SearchDC90(e, c.x, c.y);
    const int run1 = r1.x - c.x + 1;
    const SearchResult r2 = SearchD760(e, c.x, c.y);
    const int run2 = r2.y - c.y + 1;
    if (run1 < 4 || run2 < 4) {
        // Binary 0x180013200 appends when run1<2 || run2<2 || b3(x-1,y) ABSENT.
        if (run1 >= 2 && run2 >= 2 && e.Present(c.x - 1, c.y, 3)) return;
        const int mn = run1 < run2 ? run1 : run2;
        const float base = (mn == 3) ? 0.125f : 0.5f;
        const float k = base * (c.s * s_scale);
        AppendSample(c, c.x - 1, c.y, k * 0.40000001f);
        AppendSample(c, c.x - 1, c.y - 1, k * 0.2f);
        AppendSample(c, c.x, c.y - 1, k * 0.40000001f);
    }
}

// --- Stair handlers (0x180011E40/110F0/123A0/11660 + bool variants) ----------
// If the diagonal span < 4 the void forms fall back to their corner at 0.125;
// the bool forms return false (the dispatcher then runs the family main).
// With span >= 4 the ends are classified inline and 12910 computes the
// weight; 11E40/12100/113C0/12680 use out[1], 123A0/11660/11A60 use out[0].

// t1 for 11E40's up-left end (10C90 result).
int StairT1_11E40(const EdgeDesc &e, const StairPos &r1) {
    if (r1.x == 0) return 2;
    if (r1.y + 1 != 0 && !e.Present(r1.x, r1.y, 0))
        return e.Present(r1.x - 1, r1.y + 1, 1) ? 1 : 0;
    if (e.Present(r1.x, r1.y + 1, 2)) return 0;
    if (r1.y + 1 >= 2 && e.Present(r1.x, r1.y - 1, 0)) return 3;
    return 2;
}

// t2 for 11E40's down-right end (10A90 result).
int StairT2_11E40(const EdgeDesc &e, const StairPos &r2) {
    if (r2.x == e.w - 1) return 2;
    if (!e.Present(r2.x + 1, r2.y + 1, 0))
        return e.Present(r2.x + 1, r2.y + 1, 1) ? 1 : 0;
    if (e.Present(r2.x + 1, r2.y + 1, 2)) return 0;
    if (r2.y + 1 < e.h - 1 && e.Present(r2.x + 1, r2.y + 2, 0)) return 3;
    return 2;
}

// t1 for 110F0's down-left end (10990 result).
int StairT1_110F0(const EdgeDesc &e, const StairPos &r1) {
    if (r1.x == 0) return 2;
    if (!e.Present(r1.x, r1.y + 1, 0))
        return e.Present(r1.x - 1, r1.y + 1, 1) ? 1 : 0;
    if (e.Present(r1.x - 1, r1.y + 1, 3)) return 0;
    if (r1.y + 1 < e.h - 1 && e.Present(r1.x, r1.y + 2, 0)) return 3;
    return 2;
}

// t2 for 110F0's up-right end (10F00 result).
int StairT2_110F0(const EdgeDesc &e, const StairPos &r2) {
    if (r2.x == e.w - 1) return 2;
    if (!e.Present(r2.x + 1, r2.y, 0))
        return e.Present(r2.x + 1, r2.y + 1, 1) ? 1 : 0;
    if (e.Present(r2.x, r2.y + 1, 3)) return 0;
    if (r2.y + 1 >= 2 && e.Present(r2.x + 1, r2.y - 1, 0)) return 3;
    return 2;
}

// t1 for 123A0's up-left end (10DE0 result).
int StairT1_123A0(const EdgeDesc &e, const StairPos &r1) {
    if (r1.x == 0) return 2;
    if (r1.y != 0 && !e.Present(r1.x, r1.y - 1, 0))
        return e.Present(r1.x - 1, r1.y, 1) ? 1 : 0;
    if (e.Present(r1.x, r1.y, 2)) return 0;
    if (r1.y >= 2 && e.Present(r1.x, r1.y - 2, 0)) return 3;
    return 2;
}

// t2 for 123A0's down-right end (10B90 result).
int StairT2_123A0(const EdgeDesc &e, const StairPos &r2) {
    if (r2.x == e.w - 1) return 2;
    if (!e.Present(r2.x + 1, r2.y, 0))
        return e.Present(r2.x + 1, r2.y, 1) ? 1 : 0;
    if (e.Present(r2.x + 1, r2.y, 2)) return 0;
    if (r2.y < e.h - 1 && e.Present(r2.x + 1, r2.y + 1, 0)) return 3;
    return 2;
}

// t1 for 11660's down-left end (inline walk result).
int StairT1_11660(const EdgeDesc &e, const StairPos &r1) {
    if (r1.x == 0) return 2;
    if (!e.Present(r1.x, r1.y, 0))
        return e.Present(r1.x - 1, r1.y, 1) ? 1 : 0;
    if (e.Present(r1.x - 1, r1.y, 3)) return 0;
    if (r1.y < e.h - 1 && e.Present(r1.x, r1.y + 1, 0)) return 3;
    return 2;
}

// t2 for 11660's up-right end (inline walk result).
int StairT2_11660(const EdgeDesc &e, const StairPos &r2) {
    if (r2.x == e.w - 1) return 2;
    if (r2.y != 0 && !e.Present(r2.x + 1, r2.y - 1, 0))
        return e.Present(r2.x + 1, r2.y, 1) ? 1 : 0;
    if (e.Present(r2.x, r2.y, 3)) return 0;
    if (r2.y >= 2 && e.Present(r2.x + 1, r2.y - 2, 0)) return 3;
    return 2;
}

// 0x180011E40: "\" stair, sample (x, y+1), weight out[1]; case = t1 + 4*t2.
bool StairHandler11E40(MlaaCtx &c, bool fallback_corner) {
    const EdgeDesc &e = *c.edge;
    const StairPos r1 = StairSearch10C90(e, c.x, c.y);
    const StairPos r2 = StairSearch10A90(e, c.x, c.y);
    const int span = r2.x - r1.x + 1;
    if (span < 4) {
        if (!fallback_corner) return false;
        // Binary 0x180011ED9: diagonal present -> return 0; absent -> corner.
        if (e.Present(c.x, c.y + 1, 2)) return false;
        CornerDownLeft(c, 0.125f);
        return true;
    }
    const int t1 = StairT1_11E40(e, r1);
    const int t2 = StairT2_11E40(e, r2);
    float w[2];
    StairWeights12910(w, span, r2.x - c.x, t2, t1);
    const float factor = c.es * 0.25f + c.s;
    AppendSample(c, c.x, c.y + 1, w[1] * factor);
    return true;
}

// 0x1800110F0: "/" stair, sample (x, y+1), weight out[1]; case = t2 + 4*t1.
bool StairHandler110F0(MlaaCtx &c, bool fallback_corner) {
    const EdgeDesc &e = *c.edge;
    const StairPos r1 = StairSearch10990(e, c.x, c.y);
    const StairPos r2 = StairSearch10F00(e, c.x, c.y);
    const int span = r2.x - r1.x + 1;
    if (span < 4) {
        if (!fallback_corner) return false;
        // Binary 0x180011186: diagonal present -> return 0; absent -> corner.
        if (e.Present(c.x, c.y + 1, 3)) return false;
        CornerDownRight(c, 0.125f);
        return true;
    }
    const int t1 = StairT1_110F0(e, r1);
    const int t2 = StairT2_110F0(e, r2);
    float w[2];
    StairWeights12910(w, span, c.x - r1.x, t1, t2);
    const float factor = c.es * 0.25f + c.s;
    AppendSample(c, c.x, c.y + 1, w[1] * factor);
    return true;
}

// 0x1800123A0: "\" stair (vertical form), sample (x, y-1), weight out[0];
// case = t1 + 4*t2.
bool StairHandler123A0(MlaaCtx &c, bool fallback_corner) {
    const EdgeDesc &e = *c.edge;
    const StairPos r1 = StairSearch10DE0(e, c.x, c.y);
    const StairPos r2 = StairSearch10B90(e, c.x, c.y);
    const int span = r2.x - r1.x + 1;
    if (span < 4) {
        if (!fallback_corner) return false;
        if (c.x + 1 < 0 || c.x + 1 >= e.w || c.y < 0 || c.y >= e.h) return true;
        // Binary 0x18001246A: diagonal present -> return 0; absent -> corner.
        if (e.Present(c.x + 1, c.y, 2)) return false;
        CornerUpRight(c, 0.125f);
        return true;
    }
    const int t1 = StairT1_123A0(e, r1);
    const int t2 = StairT2_123A0(e, r2);
    float w[2];
    StairWeights12910(w, span, r2.x - c.x, t2, t1);
    const float factor = c.es * 0.25f + c.s;
    AppendSample(c, c.x, c.y - 1, w[0] * factor);
    return true;
}

// 0x180011660: "/" stair (vertical form, inline walks), sample (x, y-1),
// weight out[0]; case = t2 + 4*t1.
bool StairHandler11660(MlaaCtx &c, bool fallback_corner) {
    const EdgeDesc &e = *c.edge;
    // Inline walk 1: down-left from (x, y); limit = min(x, H-y-1).
    StairPos r1 = {c.x, c.y};
    {
        const int limit = std::min(c.x, e.h - c.y - 1);
        if (limit != 0 && e.Present(c.x, c.y, 0)) {
            int cx = c.x, cy = c.y;
            int walked = 0;
            bool keep_step = false;
            for (;;) {
                const int X = cx - 1, Y = cy + 1;
                // b1 absent / b3 present / b3-above present -> restore pre-step.
                if (!e.Present(X, Y, 1) || e.Present(X, Y, 3) || e.Present(X, Y - 1, 3))
                    break;
                // b0 absent or count exhausted -> keep the stepped position.
                if (!e.Present(X, Y, 0) || ++walked >= limit) {
                    r1 = {X, Y};
                    keep_step = true;
                    break;
                }
                cx = X;
                cy = Y;
            }
            if (!keep_step) r1 = {cx, cy};
        }
    }
    // Inline walk 2: up-right from (x, y). The candidate is (cx+1, cy-1);
    // every keep-step exit returns the candidate {X, Y} itself (the binary's
    // keep register holds the candidate row), and continue advances to it.
    StairPos r2 = {c.x, c.y};
    {
        int limit = e.w - c.x - 1;
        if (c.y < limit) limit = c.y;
        if (limit != 0 && !e.Present(c.x, c.y, 3) && !e.Present(c.x, c.y - 1, 3)) {
            int cx = c.x, cy = c.y;
            int walked = 0;
            bool keep_step = false;
            for (;;) {
                const int X = cx + 1, Y = cy - 1;
                if (!e.Present(X, Y, 0) || !e.Present(X, Y, 1)) break;  // restore
                if (e.Present(X, Y, 3)) {
                    r2 = {X, Y};
                    keep_step = true;
                    break;
                }
                if (e.Present(X, Y - 1, 3) || ++walked >= limit) {
                    r2 = {X, Y};
                    keep_step = true;
                    break;
                }
                cx = X;
                cy = Y;
            }
            if (!keep_step) r2 = {cx, cy};
        }
    }
    const int span = (r2.x - c.x + 1) + (c.x - r1.x);
    if (span < 4) {
        if (!fallback_corner) return false;
        if (c.x < 1) return true;
        if (e.Present(c.x - 1, c.y, 3)) return false;
        CornerUpLeft(c, 0.125f);
        return true;
    }
    const int t1 = StairT1_11660(e, r1);
    const int t2 = StairT2_11660(e, r2);
    float w[2];
    StairWeights12910(w, span, c.x - r1.x, t1, t2);
    const float factor = c.es * 0.25f + c.s;
    AppendSample(c, c.x, c.y - 1, w[0] * factor);
    return true;
}

// Bool variants (0x180012100/113C0/12680/11A60): identical to their void
// twins except the span < 4 path returns false without running the corner
// (the dispatcher then runs the family main). Weights: 12100/113C0 use
// out[1]; 12680/11A60 use out[0] (verified in disasm).

// 0x180012100: bool twin of 11E40.
bool StairHandler12100(MlaaCtx &c) {
    const EdgeDesc &e = *c.edge;
    const StairPos r1 = StairSearch10C90(e, c.x, c.y);
    const StairPos r2 = StairSearch10A90(e, c.x, c.y);
    const int span = r2.x - r1.x + 1;
    if (span < 4) return false;
    const int t1 = StairT1_11E40(e, r1);
    const int t2 = StairT2_11E40(e, r2);
    float w[2];
    StairWeights12910(w, span, r2.x - c.x, t2, t1);
    const float factor = c.es * 0.25f + c.s;
    AppendSample(c, c.x, c.y + 1, w[1] * factor);
    return true;
}

// 0x1800113C0: bool twin of 110F0.
bool StairHandler113C0(MlaaCtx &c) {
    const EdgeDesc &e = *c.edge;
    const StairPos r1 = StairSearch10990(e, c.x, c.y);
    const StairPos r2 = StairSearch10F00(e, c.x, c.y);
    const int span = r2.x - r1.x + 1;
    if (span < 4) return false;
    const int t1 = StairT1_110F0(e, r1);
    const int t2 = StairT2_110F0(e, r2);
    float w[2];
    StairWeights12910(w, span, c.x - r1.x, t1, t2);
    const float factor = c.es * 0.25f + c.s;
    AppendSample(c, c.x, c.y + 1, w[1] * factor);
    return true;
}

// 0x180012680: bool twin of 123A0.
bool StairHandler12680(MlaaCtx &c) {
    const EdgeDesc &e = *c.edge;
    const StairPos r1 = StairSearch10DE0(e, c.x, c.y);
    const StairPos r2 = StairSearch10B90(e, c.x, c.y);
    const int span = r2.x - r1.x + 1;
    if (span < 4) return false;
    const int t1 = StairT1_123A0(e, r1);
    const int t2 = StairT2_123A0(e, r2);
    float w[2];
    StairWeights12910(w, span, r2.x - c.x, t2, t1);
    const float factor = c.es * 0.25f + c.s;
    AppendSample(c, c.x, c.y - 1, w[0] * factor);
    return true;
}

// 0x180011A60: bool twin of 11660 (inline walks).
bool StairHandler11A60(MlaaCtx &c) {
    const EdgeDesc &e = *c.edge;
    StairPos r1 = {c.x, c.y};
    {
        const int limit = std::min(c.x, e.h - c.y - 1);
        if (limit != 0 && e.Present(c.x, c.y, 0)) {
            int cx = c.x, cy = c.y;
            int walked = 0;
            bool keep_step = false;
            for (;;) {
                const int X = cx - 1, Y = cy + 1;
                if (!e.Present(X, Y, 1) || e.Present(X, Y, 3) || e.Present(X, Y - 1, 3))
                    break;
                if (!e.Present(X, Y, 0) || ++walked >= limit) {
                    r1 = {X, Y};
                    keep_step = true;
                    break;
                }
                cx = X;
                cy = Y;
            }
            if (!keep_step) r1 = {cx, cy};
        }
    }
    StairPos r2 = {c.x, c.y};
    {
        int limit = e.w - c.x - 1;
        if (c.y < limit) limit = c.y;
        if (limit != 0 && !e.Present(c.x, c.y, 3) && !e.Present(c.x, c.y - 1, 3)) {
            int cx = c.x, cy = c.y;
            int walked = 0;
            bool keep_step = false;
            for (;;) {
                const int X = cx + 1, Y = cy - 1;
                if (!e.Present(X, Y, 0) || !e.Present(X, Y, 1)) break;
                if (e.Present(X, Y, 3)) {
                    r2 = {X, Y};
                    keep_step = true;
                    break;
                }
                if (e.Present(X, Y - 1, 3) || ++walked >= limit) {
                    r2 = {X, Y};
                    keep_step = true;
                    break;
                }
                cx = X;
                cy = Y;
            }
            if (!keep_step) r2 = {cx, cy};
        }
    }
    const int span = (r2.x - c.x + 1) + (c.x - r1.x);
    if (span < 4) return false;
    const int t1 = StairT1_11660(e, r1);
    const int t2 = StairT2_11660(e, r2);
    float w[2];
    StairWeights12910(w, span, c.x - r1.x, t1, t2);
    const float factor = c.es * 0.25f + c.s;
    AppendSample(c, c.x, c.y - 1, w[0] * factor);
    return true;
}

// --- 0x18000C340: the per-pixel dispatcher -----------------------------------

void MlaaDispatch(MlaaCtx &c) {
    const EdgeDesc &e = *c.edge;
    const float s = c.s;
    if (s == 0.0f) {
        c.count = 0;
        std::memset(c.samples, 0, sizeof(c.samples));
        return;
    }
    c.count = 0;
    const int x = c.x, y = c.y;
    const bool b0 = e.Present(x, y, 0);
    const bool b1 = e.Present(x, y, 1);
    const bool b2 = e.Present(x, y, 2);
    const bool b3 = e.Present(x, y, 3);
    const bool n4 = (x >= e.w - 1) || !e.Present(x + 1, y, 0);
    // Last row (y >= h-1): the binary skips the n5/n6/n7 block with n5=0, n6=0,
    // n7=1 (asm 0x18000C486/0x18000C4EA), i.e. bits 32, 64 AND 128 all set.
    bool n5 = false, n6 = false, n7 = false;
    if (y < e.h - 1) {
        n5 = (x >= 1) && e.Present(x - 1, y + 1, 3);
        n6 = e.Present(x, y + 1, 1);
        n7 = (x < e.w - 1) && e.Present(x + 1, y + 1, 2);
    }
    int idx = 0;
    if (!b2) idx |= 1;
    if (!b1) idx |= 2;
    if (!b3) idx |= 4;
    if (!b0) idx |= 8;
    if (n4) idx |= 16;
    if (!n5) idx |= 32;
    if (!n6) idx |= 64;
    if (!n7) idx |= 128;

    switch (idx) {
        case 0:
            CornerUpLeft(c, 0.125f);
            CornerUpRight(c, 0.125f);
            CornerDownRight(c, 0.125f);
            CornerDownLeft(c, 0.125f);
            break;
        case 1:
            StairHandler123A0(c, true);
            CornerDownRight(c, 0.125f);
            StairHandler11E40(c, true);
            break;
        case 2: case 64: case 66: case 67: case 69: case 70: case 71:
        case 98: case 102: case 162: case 194: case 195: case 226:
            // Binary 0x18000C5F2: FamilyB falls through into FamilyC.
            FamilyBMain(c);
            FamilyCMain(c);
            break;
        case 3: case 131:
            StairHandler11E40(c, true);
            FamilyCMain(c);
            break;
        case 4:
            StairHandler11660(c, true);
            CornerDownLeft(c, 0.125f);
            StairHandler110F0(c, true);
            break;
        case 5: case 7: case 39: case 135: case 167:
            StairHandler11E40(c, true);
            StairHandler110F0(c, true);
            break;
        case 6: case 38:
            StairHandler110F0(c, true);
            FamilyBMain(c);
            break;
        case 8: case 16: case 24: case 25: case 28: case 49: case 56: case 57:
        case 60: case 140: case 152: case 153: case 156:
            // Binary 0x18000C889: FamilyD falls through into FamilyA (0x18000C893).
            FamilyDMain(c);
            FamilyAMain(c);
            break;
        case 9: case 137:
            StairHandler123A0(c, true);
            FamilyAMain(c);
            break;
        case 10: case 138:
            RunGatedDownRight(c, 0.5f);
            break;
        case 11: case 139:
            RunGatedDownRight(c, 1.0f);
            break;
        case 12:
            if (!StairHandler113C0(c)) FamilyAMain(c);
            FamilyDMain(c);
            break;
        case 13: case 14: case 15: case 142: case 143:
            StairHandler113C0(c);
            FamilyAMain(c);
            NormalizeSamples(c);
            break;
        case 17:
            if (!StairHandler12100(c)) FamilyAMain(c);
            FamilyDMain(c);
            break;
        case 18: case 50:
            RunGatedDownLeft(c, 0.5f);
            break;
        case 19: case 21: case 23: case 51: case 55:
            StairHandler12100(c);
            FamilyAMain(c);
            NormalizeSamples(c);
            break;
        case 20: case 52:
            StairHandler11660(c, true);
            FamilyAMain(c);
            break;
        case 22: case 54:
            RunGatedDownLeft(c, 1.0f);
            break;
        case 26: case 27: case 29: case 30: case 31: case 53: case 58: case 59:
        case 61: case 62: case 63: case 141: case 154: case 155: case 157:
        case 158: case 159:
            FamilyAMain(c);
            break;
        case 32:
            StairHandler11660(c, true);
            CornerUpRight(c, 0.125f);
            StairHandler110F0(c, true);
            break;
        case 33: case 41: case 45: case 169: case 173:
            StairHandler123A0(c, true);
            StairHandler110F0(c, true);
            break;
        case 34:
            if (StairHandler113C0(c)) {
                FamilyBMain(c);
            } else {
                FamilyCMain(c);
                FamilyBMain(c);
            }
            break;
        case 35: case 42: case 43: case 170: case 171:
            // Binary 0x18000CA83: 113C0; FamilyC; Normalize (no FamilyB).
            StairHandler113C0(c);
            FamilyCMain(c);
            NormalizeSamples(c);
            break;
        case 36:
            StairHandler11660(c, true);
            StairHandler110F0(c, true);
            break;
        case 37: case 133: case 161: case 164: case 165:
            StairHandler11660(c, true);
            StairHandler110F0(c, true);
            StairHandler123A0(c, true);
            StairHandler11E40(c, true);
            break;
        case 40: case 44:
            FamilyDMain(c);
            StairHandler110F0(c, true);
            break;
        case 46: case 47: case 174: case 175:
            StairHandler110F0(c, true);
            break;
        case 48:
            if (StairHandler11A60(c)) {
                FamilyAMain(c);
            } else {
                FamilyDMain(c);
                FamilyAMain(c);
            }
            break;
        case 65:
            if (StairHandler12680(c)) {
                FamilyBMain(c);
            } else {
                FamilyCMain(c);
                FamilyBMain(c);
            }
            break;
        case 68:
            if (StairHandler11A60(c)) {
                FamilyCMain(c);
            } else {
                FamilyBMain(c);
                FamilyCMain(c);
            }
            break;
        case 72: case 76:
            RunGatedUpLeft(c, 0.5f);
            break;
        case 73: case 77: case 97: case 105: case 109:
            StairHandler12680(c);
            FamilyCMain(c);
            NormalizeSamples(c);
            break;
        case 74: case 75: case 78: case 79: case 99: case 101: case 103:
        case 106: case 107: case 110: case 111: case 163: case 202: case 203:
        case 227: case 234: case 235:
            FamilyCMain(c);
            break;
        case 80: case 81:
            RunGatedUpRight(c, 0.5f);
            break;
        case 82: case 83: case 86: case 87: case 114: case 118: case 166:
        case 197: case 198: case 199: case 210: case 211: case 214: case 215:
        case 230: case 242: case 246:
            FamilyBMain(c);
            break;
        case 84: case 85: case 196: case 212: case 213:
            StairHandler11A60(c);
            FamilyBMain(c);
            NormalizeSamples(c);
            break;
        case 88: case 89: case 92: case 120: case 121: case 124: case 172:
        case 177: case 184: case 185: case 188: case 216: case 217: case 220:
        case 248: case 249: case 252:
            FamilyDMain(c);
            break;
        case 96: case 100:
            StairHandler11660(c, true);
            FamilyCMain(c);
            break;
        case 104: case 108:
            RunGatedUpLeft(c, 1.0f);
            break;
        case 112: case 113: case 176: case 240: case 241:
            StairHandler11A60(c);
            FamilyDMain(c);
            NormalizeSamples(c);
            break;
        case 116: case 117: case 244: case 245:
            StairHandler11660(c, true);
            break;
        case 128:
            StairHandler123A0(c, true);
            CornerUpLeft(c, 0.125f);
            StairHandler11E40(c, true);
            break;
        case 129:
            StairHandler123A0(c, true);
            StairHandler11E40(c, true);
            break;
        case 130:
            if (StairHandler12100(c)) {
                FamilyCMain(c);
            } else {
                FamilyBMain(c);
                FamilyCMain(c);
            }
            break;
        case 132: case 148: case 149: case 180: case 181:
            StairHandler11660(c, true);
            StairHandler11E40(c, true);
            break;
        case 134: case 146: case 150: case 178: case 182:
            StairHandler12100(c);
            FamilyBMain(c);
            NormalizeSamples(c);
            break;
        case 136:
            if (StairHandler12680(c)) {
                FamilyAMain(c);
            } else {
                FamilyDMain(c);
                FamilyAMain(c);
            }
            break;
        case 144: case 145:
            FamilyDMain(c);
            StairHandler11E40(c, true);
            break;
        case 147: case 151: case 179: case 183:
            StairHandler11E40(c, true);
            break;
        case 160: case 224: case 225: case 228: case 229:
            StairHandler11660(c, true);
            StairHandler123A0(c, true);
            break;
        case 168: case 200: case 204: case 232: case 236:
            StairHandler12680(c);
            FamilyDMain(c);
            NormalizeSamples(c);
            break;
        case 192: case 193:
            FamilyBMain(c);
            StairHandler123A0(c, true);
            break;
        case 201: case 205: case 233: case 237:
            StairHandler123A0(c, true);
            break;
        case 208: case 209:
            RunGatedUpRight(c, 1.0f);
            break;
        default:
            break;
    }
}

// --- Blend (0x18000BBD0/C190/ABC0/B1E0) --------------------------------------

struct GammaInfo {
    const float (*colors)[3];
    int count;
    bool encode;   // !v1: test colors are linear -> encode to sRGB first
};

// 0x18000AA80: encode the copy's RGB (when needed) and match against the
// gamma color list with the 1/510 test. Operates on a local copy only.
bool GammaColorMatch(const GammaInfo &gi, const float *rgba) {
    float rgb[3];
    if (gi.encode) {
        rgb[0] = SrgbEncode(rgba[0]);
        rgb[1] = SrgbEncode(rgba[1]);
        rgb[2] = SrgbEncode(rgba[2]);
    } else {
        rgb[0] = rgba[0];
        rgb[1] = rgba[1];
        rgb[2] = rgba[2];
    }
    for (int i = 0; i < gi.count; ++i) {
        if (fabsf(rgb[0] - gi.colors[i][0]) < kMatchEps &&
            fabsf(rgb[1] - gi.colors[i][1]) < kMatchEps &&
            fabsf(rgb[2] - gi.colors[i][2]) < kMatchEps)
            return true;
    }
    return false;
}

// 0x18000BBD0: decide gamma apply; output = {gamma value, apply flag}.
void BlendDecide(const OLMS2Params &p, const GammaInfo &gi, const float *pixel,
                 const float samples[][5], int count, float *gamma_out, bool *apply_out) {
    // a2 == 0 (no gamma context) and tag 0 both leave apply false. The tag-2
    // luma-curve path is unreachable in the live pipeline (the context tag is
    // only ever 0/1/3 from 0x180004E10), so it is not ported.
    bool apply = false;
    if (p.gamma_mode == OLMS2_GAMMA_ALL) {
        apply = true;
    } else if (p.gamma_mode == OLMS2_GAMMA_COLORS && gi.count > 0) {
        apply = GammaColorMatch(gi, pixel);
        for (int i = 0; !apply && i < count; ++i)
            apply = GammaColorMatch(gi, samples[i]);
    }
    *gamma_out = p.gamma_value;
    *apply_out = apply;
}

// 0x18000C190: gamma-encode (double pow) then premultiply pixel and samples.
void BlendEncodePremul(float *pixel, float samples[][5], int count, float gamma, bool apply) {
    if (apply) {
        const double ig = (double)(float)(1.0 / (double)gamma);
        for (int i = 0; i < 3; ++i) pixel[i] = (float)std::pow((double)pixel[i], ig);
        for (int s = 0; s < count; ++s)
            for (int i = 0; i < 3; ++i)
                samples[s][i] = (float)std::pow((double)samples[s][i], ig);
    }
    if (pixel[3] != 1.0f) {
        for (int i = 0; i < 3; ++i) pixel[i] *= pixel[3];
    }
    for (int s = 0; s < count; ++s) {
        if (samples[s][3] != 1.0f) {
            for (int i = 0; i < 3; ++i) samples[s][i] *= samples[s][3];
        }
    }
}

// 0x18000ABC0: weighted blend; out = (1-Wc)*pixel + sum(w_i * sample_i).
void BlendMix(const float *pixel, const float samples[][5], int count, float *out) {
    if (count == 0) {
        out[0] = pixel[0];
        out[1] = pixel[1];
        out[2] = pixel[2];
        out[3] = pixel[3];
        return;
    }
    float wsum = 0.0f;
    for (int i = 0; i < count; ++i) wsum += samples[i][4];
    const float wc = wsum >= 0.0f ? std::min(1.0f, wsum) : 0.0f;
    for (int ch = 0; ch < 4; ++ch) {
        float acc = (1.0f - wc) * pixel[ch];
        for (int i = 0; i < count; ++i) acc += samples[i][4] * samples[i][ch];
        out[ch] = acc;
    }
}

// 0x18000B1E0: unpremultiply, gamma-decode (double pow), clamp.
void BlendFinish(float *v, float gamma, bool apply) {
    const float a = v[3];
    if (a != 0.0f && a != 1.0f) {
        for (int i = 0; i < 3; ++i) v[i] /= a;
    }
    if (apply) {
        for (int i = 0; i < 3; ++i)
            v[i] = (float)std::pow((double)v[i], (double)gamma);
    }
    // Clamp as the binary does (probed on 0x18000B1E0): x < 0 -> 0, else MSVC
    // fminf(1, x), which passes NaN through (-0 and NaN are kept, +inf -> 1).
    for (int i = 0; i < 3; ++i) v[i] = (v[i] < 0.0f) ? 0.0f : ((1.0f < v[i]) ? 1.0f : v[i]);
    v[3] = (a < 0.0f) ? 0.0f : ((1.0f < a) ? 1.0f : a);
}

// --- Per-pixel glue (0x18000CDA0) + writers (0x180003370/36E0/3990) ----------

void WritePixel(const OLMS2Params &p, const float *rgba, int depth, PF_EffectWorld &out,
                int x, int y) {
    float r = rgba[0], g = rgba[1], b = rgba[2], a = rgba[3];
    if (!p.v1) {
        r = SrgbEncode(r);
        g = SrgbEncode(g);
        b = SrgbEncode(b);
    }
    // Dead premultiply-on-output flag (ctx byte 25) is always 0 -> skipped.
    if (depth == 8) {
        PF_Pixel *o = (PF_Pixel *)((char *)out.data + (size_t)out.rowbytes * (size_t)y +
                                   (size_t)x * 4);
        o->alpha = (A_u_char)(int)((a * 255.0f) + 0.5f);
        o->red = (A_u_char)(int)((r * 255.0f) + 0.5f);
        o->green = (A_u_char)(int)((g * 255.0f) + 0.5f);
        o->blue = (A_u_char)(int)((b * 255.0f) + 0.5f);
    } else if (depth == 16) {
        PF_Pixel16 *o = (PF_Pixel16 *)((char *)out.data + (size_t)out.rowbytes * (size_t)y +
                                       (size_t)x * 8);
        o->alpha = (A_u_short)(int)((a * 32768.0f) + 0.5f);
        o->red = (A_u_short)(int)((r * 32768.0f) + 0.5f);
        o->green = (A_u_short)(int)((g * 32768.0f) + 0.5f);
        o->blue = (A_u_short)(int)((b * 32768.0f) + 0.5f);
    } else {
        PF_PixelFloat *o = (PF_PixelFloat *)((char *)out.data +
                                             (size_t)out.rowbytes * (size_t)y +
                                             (size_t)x * 16);
        o->alpha = a;
        o->red = r;
        o->green = g;
        o->blue = b;
    }
}

void ProcessPixel(const OLMS2Params &p, const GammaInfo &gi, const CanvasDesc &canvas,
                  const EdgeDesc &edge, int x, int y, int depth, PF_EffectWorld &out) {
    MlaaCtx c;
    c.canvas = &canvas;
    c.edge = &edge;
    c.x = x;
    c.y = y;
    c.s = (float)p.smoothness / 100.0f;
    c.es = (float)p.extra_smooth / 100.0f;
    c.count = 0;
    std::memset(c.samples, 0, sizeof(c.samples));

    MlaaDispatch(c);

    const float *pixel = canvas.Pixel(x, y);
    float result[4];
    if (c.count != 0) {
        float gamma;
        bool apply;
        BlendDecide(p, gi, pixel, c.samples, c.count, &gamma, &apply);
        float work[4] = {pixel[0], pixel[1], pixel[2], pixel[3]};
        BlendEncodePremul(work, c.samples, c.count, gamma, apply);
        float mixed[4];
        BlendMix(work, c.samples, c.count, mixed);
        BlendFinish(mixed, gamma, apply);
        result[0] = mixed[0];
        result[1] = mixed[1];
        result[2] = mixed[2];
        result[3] = mixed[3];
    } else {
        result[0] = pixel[0];
        result[1] = pixel[1];
        result[2] = pixel[2];
        result[3] = pixel[3];
    }
    WritePixel(p, result, depth, out, x, y);
}

// --- Render ------------------------------------------------------------------

PF_Err SmartRender(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra) {
    PF_EffectWorld *input = nullptr;
    PF_EffectWorld *output = nullptr;
    const PF_Err input_err = extra->cb->checkout_layer_pixels(in_data->effect_ref,
                                                              OLMS2_INPUT, &input);
    const PF_Err output_err = extra->cb->checkout_output(in_data->effect_ref, &output);
    PF_Err err = input_err ? input_err : output_err;
    const bool checked_out = input_err == PF_Err_NONE;
    if (!err && (!input || !output)) err = PF_Err_BAD_CALLBACK_PARAM;

    try {
        OLMS2Params params;
        if (!err) err = CheckoutParams(in_data, out_data, extra->input->bitdepth, &params);
        if (!err && (!in_data->utils || !in_data->utils->copy))
            err = PF_Err_BAD_CALLBACK_PARAM;
        if (!err) err = in_data->utils->copy(in_data->effect_ref, input, output, nullptr,
                                             nullptr);
        // The binary's kernel throws "Input images do not have the same size." when
        // input and output dimensions differ; EffectMain swallows it, so the output
        // keeps the copy and no error is returned. Match that (no render, no OOB write).
        if (!err && output->width == input->width && output->height == input->height) {
            const A_long width = input->width;
            const A_long height = input->height;
            AEFX_SuiteScoper<PF_WorldSuite2> world_suite(in_data, kPFWorldSuite,
                                                         kPFWorldSuiteVersion2, out_data);
            ScopedWorld canvas_world;
            err = canvas_world.Create(in_data, world_suite.get(), width, height,
                                      PF_PixelFormat_ARGB128);
            if (!err) {
                ScopedWorld edge_world;
                err = edge_world.Create(in_data, world_suite.get(), width, height,
                                        PF_PixelFormat_ARGB32);
                if (!err) {
                    CanvasDesc canvas;
                    canvas.data = (float *)canvas_world.get()->data;
                    canvas.w = (int)width;
                    canvas.h = (int)height;
                    canvas.stride = canvas_world.get()->rowbytes;
                    EdgeDesc edge;
                    edge.data = (A_u_char *)edge_world.get()->data;
                    edge.w = (int)width;
                    edge.h = (int)height;
                    edge.stride = edge_world.get()->rowbytes;

                    switch (extra->input->bitdepth) {
                        case 8:
                            ConvertToCanvas<PF_Pixel>(*input, canvas);
                            break;
                        case 16:
                            ConvertToCanvas<PF_Pixel16>(*input, canvas);
                            break;
                        default:
                            ConvertToCanvas<PF_PixelFloat>(*input, canvas);
                            break;
                    }
                    ApplyKey(canvas, params);
                    ApplyGammaDecode(canvas, params);
                    BuildEdges(canvas, edge, params.smooth_range);

                    GammaInfo gi;
                    gi.colors = params.gamma_colors;
                    gi.count = params.gamma_mode == OLMS2_GAMMA_COLORS
                                   ? params.gamma_count
                                   : 0;
                    gi.encode = !params.v1;
                    if (gi.encode) BuildLuts();

                    for (A_long y = 0; y < height && !err; ++y) {
                        for (A_long x = 0; x < width; ++x) {
                            ProcessPixel(params, gi, canvas, edge, (int)x, (int)y,
                                         extra->input->bitdepth, *output);
                        }
                    }
                }
            }
        }
    } catch (...) {
        // Suite-acquisition and allocation failures must not skip the layer
        // checkin; rethrow so EffectMain keeps translating the original error.
        if (checked_out)
            extra->cb->checkin_layer_pixels(in_data->effect_ref, OLMS2_INPUT);
        throw;
    }
    // The original never calls checkin_layer_pixels (the host reclaims at
    // frame end); the port checks in to keep checkout accounting balanced.
    if (checked_out) {
        const PF_Err checkin_err = extra->cb->checkin_layer_pixels(in_data->effect_ref,
                                                                   OLMS2_INPUT);
        if (!err) err = checkin_err;
    }
    return err;
}

// --- PreRender ---------------------------------------------------------------

PF_Err PreRender(PF_InData *in_data, PF_PreRenderExtra *extra) {
    PF_CheckoutResult result;
    AEFX_CLR_STRUCT(result);
    PF_RenderRequest request = extra->input->output_request;
    PF_Err err = extra->cb->checkout_layer(in_data->effect_ref, OLMS2_INPUT, 0, &request,
                                           in_data->current_time, in_data->time_step,
                                           in_data->time_scale, &result);
    if (!err) {
        // The binary unions the checked-out rects into the output rects and
        // sets no output flags and no pre_render_data.
        UnionLRect(&result.result_rect, &extra->output->result_rect);
        UnionLRect(&result.max_result_rect, &extra->output->max_result_rect);
    }
    return err;
}

}  // namespace

PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
                  PF_ParamDef *params[], PF_LayerDef *output, void *extra) {
    (void)output;
    try {
        switch (cmd) {
            case PF_Cmd_ABOUT:
                return About(in_data, out_data);
            case PF_Cmd_GLOBAL_SETUP:
                return GlobalSetup(in_data, out_data);
            case PF_Cmd_PARAMS_SETUP:
                return ParamsSetup(in_data, out_data);
            case PF_Cmd_UPDATE_PARAMS_UI:
                return UpdateParamsUI(in_data, out_data, params);
            case PF_Cmd_SMART_PRE_RENDER:
                return PreRender(in_data, (PF_PreRenderExtra *)extra);
            case PF_Cmd_SMART_RENDER:
                return SmartRender(in_data, out_data, (PF_SmartRenderExtra *)extra);
            case PF_Cmd_RENDER:
            default:
                // The binary no-ops every other command (USER_CHANGED_PARAM
                // included).
                return PF_Err_NONE;
        }
    } catch (PF_Err err) {
        return err;
    } catch (const std::bad_alloc &) {
        return PF_Err_OUT_OF_MEMORY;
    } catch (...) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
}

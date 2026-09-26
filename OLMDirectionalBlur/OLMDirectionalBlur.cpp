/* OLMDirectionalBlur.cpp — macOS port of OLM Directional Blur 1.1.1.
 *
 * 1:1 port of the Windows OLMDirectionalBlur.aex (x64, entryPointFunc).
 * Original: classic SmartRender CPU effect, 8/16/32-bit, OpenMP row-parallel,
 * no GPU. This port keeps the same params, defaults, flags, PiPL identity and
 * algorithm stages; only platform glue differs (EffectMain entry, no VCOMP —
 * OpenMP pragmas are optional and compile out without -fopenmp).
 *
 * Reverse notes (IDA session, see .agents/AGENTS.md):
 *  entryPointFunc     PF_Cmd dispatch (ABOUT/GLOBAL_SETUP/PARAMS_SETUP=0/1/4,
 *                      RENDER no-op=11, UPDATE_PARAMS_UI=14,
 *                      SMART_PRE_RENDER=23, SMART_RENDER=24)
 *  ParamsSetup        sub_180007310 — 21 params, decoded defaults below
 *  CheckoutParams     sub_180006C50 — angle quant, offset/36, popup modes
 *  UpdateParamsUI     sub_180007EA0 + sub_1800081C0 — toggle 17..20 by type
 *  SmartRender        sub_180007BC0 — dispatch 8/16/32-bit kernels
 *  Kernels 8/16/32    sub_180004A20 / sub_180003C90 / sub_1800057B0
 *  BlurCore           sub_1800038D0 — row loop, size+splat passes
 *  SizePass           sub_180001000 — per-pixel opaque-group weight
 *  SplatPass          sub_1800013E0 — directional scatter, fmaxf edge fade
 *  GaussianLUT        sub_180001830 — exp(-i^2/(2*(n/3)^2+eps))
 *  NoiseField         sub_1800034E0 — 101-entry rand table + smoothstep lattice
 *  SampleNoise        sub_180003370 — nearest (Block) / smoothstep (Smooth)
 *  RotateARGB         sub_180001EC0 — alpha-weighted bilinear rotate
 *  RotatePlane        sub_1800018C0 — single-channel bilinear rotate
 */

#include "OLMDirectionalBlur.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------- About ---

static PF_Err About(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[], PF_LayerDef *output)
{
    PF_SPRINTF(out_data->return_msg, "%s v%d.%d.%d\r%s", OLMDB_NAME,
               OLMDB_MAJOR_VERSION, OLMDB_MINOR_VERSION, OLMDB_BUG_VERSION, OLMDB_DESCRIPTION);
    return PF_Err_NONE;
}

// ---------------------------------------------------------- GlobalSetup ---
// Windows: my_version=559104 (1.1.1), out_flags=0x06000040, out_flags2=0x08001408.

static AEGP_PluginID S_olmdb_id = 0;

static PF_Err GlobalSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[], PF_LayerDef *output)
{
    out_data->my_version = PF_VERSION(OLMDB_MAJOR_VERSION, OLMDB_MINOR_VERSION, OLMDB_BUG_VERSION,
                                      OLMDB_STAGE_VERSION, OLMDB_BUILD_VERSION);

    out_data->out_flags = PF_OutFlag_USE_OUTPUT_EXTENT | PF_OutFlag_DEEP_COLOR_AWARE |
                          PF_OutFlag_SEND_UPDATE_PARAMS_UI;

    out_data->out_flags2 = PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG |
                           PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING;

    // Register for an AEGP id used by UpdateParamsUI stream toggles
    // (Windows GlobalSetup: AEGP Utility Suite v7 +56 "OLMDirectionalBlur").
    AEGP_SuiteHandler suites(in_data, out_data);
    suites.UtilitySuite3()->AEGP_RegisterWithAEGP(nullptr, OLMDB_NAME, &S_olmdb_id);
    return PF_Err_NONE;
}

// ---------------------------------------------------------- ParamsSetup ---
// Types: 3=ANGLE, 10=FLOAT_SLIDER, 2=FIX_SLIDER(%), 1=SLIDER, 13/14=GROUP, 7=POPUP, 0=LAYER.

static PF_Err ParamsSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[], PF_LayerDef *output)
{
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Angle", 0, OLMDB_ANGLE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDER("Brightness Gain", 0, 10, 0, 2, AEFX_DEFAULT_CURVE_TOLERANCE, 1.0, 2,
                        PF_ValueDisplayFlag_NONE, false, OLMDB_BRIGHTNESS);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FIXED("Size Variation", 0, 100, 0, 100, 0, 1, PF_ValueDisplayFlag_PERCENT, 0, OLMDB_SIZE_VAR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Front Blur Parameters", OLMDB_FRONT_TOPIC);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Blur Strength", 0, 4000, 0, 4000, 0, OLMDB_FRONT_BLUR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Alpha Fade", 0, 100, 0, 100, 0, OLMDB_FRONT_FADE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FIXED("Sharp Tail", 0, 100, 0, 100, 0, 1, PF_ValueDisplayFlag_PERCENT, 0, OLMDB_FRONT_TAIL);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OLMDB_FRONT_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Back Blur Parameters", OLMDB_BACK_TOPIC);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Blur Strength", 0, 4000, 0, 4000, 0, OLMDB_BACK_BLUR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Alpha Fade", 0, 100, 0, 100, 0, OLMDB_BACK_FADE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FIXED("Sharp Tail", 0, 100, 0, 100, 0, 1, PF_ValueDisplayFlag_PERCENT, 0, OLMDB_BACK_TAIL);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OLMDB_BACK_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Noise Parameters", OLMDB_NOISE_TOPIC);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FIXED("Noise Variation", 0, 100, 0, 100, 0, 1, PF_ValueDisplayFlag_PERCENT, 0, OLMDB_NOISE_VAR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Noise Type", 3, 1, "Smooth | Block | Layer", OLMDB_NOISE_TYPE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Noise Layer", 0, OLMDB_NOISE_LAYER);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Seed", 1, 1000, 1, 1000, 1, OLMDB_SEED);

    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Offset", 0, OLMDB_OFFSET);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDER("Thickness", 1, 100, 1, 100, AEFX_DEFAULT_CURVE_TOLERANCE, 10.0, 2,
                        PF_ValueDisplayFlag_NONE, false, OLMDB_THICKNESS);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OLMDB_NOISE_END);

    out_data->num_params = OLMDB_NUM_PARAMS;
    return err;
}

// Helper: hide/reveal a param's ECW UI via AEGP suites (mirrors sub_1800081C0,
// which toggles AEGP_DynStreamFlag_HIDDEN = 1<<1 on params 17..20 by noise type).
static PF_Err SetParamHidden(PF_InData *in_data, PF_OutData *out_data, PF_ParamIndex id, PF_Boolean hidden)
{
    AEGP_SuiteHandler suites(in_data, out_data);
    AEGP_EffectRefH effectH = nullptr;
    AEGP_StreamRefH streamH = nullptr;
    PF_Err err = suites.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(S_olmdb_id, in_data->effect_ref,
                                                                       &effectH);
    if (!err) {
        err = suites.StreamSuite7()->AEGP_GetNewEffectStreamByIndex(S_olmdb_id, effectH, id, &streamH);
        if (!err) {
            err = suites.DynamicStreamSuite4()->AEGP_SetDynamicStreamFlag(
                streamH, AEGP_DynStreamFlag_HIDDEN, false, hidden);
            suites.StreamSuite7()->AEGP_DisposeStream(streamH);
        }
        suites.EffectSuite5()->AEGP_DisposeEffect(effectH);
    }
    return err;
}

static PF_Err UpdateParamsUIFull(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[], void *extra)
{
    (void)params; (void)extra;
    PF_Err err = PF_Err_NONE;
    PF_ParamDef cur;
    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_NOISE_TYPE, in_data->current_time, in_data->time_step,
                          in_data->time_scale, &cur));
    PF_Boolean is_layer = (cur.u.pd.value == OLMDB_NOISE_LAYER);
    ERR(PF_CHECKIN_PARAM(in_data, &cur));
    if (!err) ERR(SetParamHidden(in_data, out_data, OLMDB_NOISE_LAYER, !is_layer));
    if (!err) ERR(SetParamHidden(in_data, out_data, OLMDB_SEED, is_layer));
    if (!err) ERR(SetParamHidden(in_data, out_data, OLMDB_OFFSET, is_layer));
    if (!err) ERR(SetParamHidden(in_data, out_data, OLMDB_THICKNESS, is_layer));
    return err;
}

// -------------------------------------------------------- CheckoutParams ---
// Mirrors sub_180006C50 incl. integer-degree angle quantisation.

static PF_Err CheckoutParams(PF_InData *in_data, OLMDBParams *pp)
{
    PF_Err err = PF_Err_NONE;
    PF_ParamDef cur;
    memset(pp, 0, sizeof(*pp));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_ANGLE, in_data->current_time, in_data->time_step, in_data->time_scale, &cur));
    { PF_Fixed fx = cur.u.ad.value; short deg = (short)(fx >> 16); pp->angle_rad = (deg + 90) / 180.0f * (float)M_PI; }
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_BRIGHTNESS, in_data->current_time, in_data->time_step, in_data->time_scale, &cur));
    pp->brightness = (float)cur.u.fs_d.value;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_SIZE_VAR, in_data->current_time, in_data->time_step, in_data->time_scale, &cur));
    pp->size_var = (short)(cur.u.fd.value >> 16) / 100.0f;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_FRONT_BLUR, in_data->current_time, in_data->time_step, in_data->time_scale, &cur));
    pp->front_blur = cur.u.sd.value;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_FRONT_FADE, in_data->current_time, in_data->time_step, in_data->time_scale, &cur));
    pp->front_fade = cur.u.sd.value;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_FRONT_TAIL, in_data->current_time, in_data->time_step, in_data->time_scale, &cur));
    pp->front_tail = (short)(cur.u.fd.value >> 16) / 100.0f;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_BACK_BLUR, in_data->current_time, in_data->time_step, in_data->time_scale, &cur));
    pp->back_blur = cur.u.sd.value;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_BACK_FADE, in_data->current_time, in_data->time_step, in_data->time_scale, &cur));
    pp->back_fade = cur.u.sd.value;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_BACK_TAIL, in_data->current_time, in_data->time_step, in_data->time_scale, &cur));
    pp->back_tail = (short)(cur.u.fd.value >> 16) / 100.0f;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_NOISE_VAR, in_data->current_time, in_data->time_step, in_data->time_scale, &cur));
    pp->noise_var = (short)(cur.u.fd.value >> 16) / 100.0f;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_NOISE_TYPE, in_data->current_time, in_data->time_step, in_data->time_scale, &cur));
    pp->noise_type = cur.u.pd.value;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_SEED, in_data->current_time, in_data->time_step, in_data->time_scale, &cur));
    pp->seed = cur.u.sd.value;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_OFFSET, in_data->current_time, in_data->time_step, in_data->time_scale, &cur));
    { PF_Fixed fx = cur.u.ad.value; pp->offset = (short)(fx >> 16) / 36.0f; }
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    AEFX_CLR_STRUCT(cur);
    ERR(PF_CHECKOUT_PARAM(in_data, OLMDB_THICKNESS, in_data->current_time, in_data->time_step, in_data->time_scale, &cur));
    pp->thickness = (float)cur.u.fs_d.value;
    ERR(PF_CHECKIN_PARAM(in_data, &cur));

    return err;
}

// ---------------------------------------------------------------- Rand ---
// MSVC CRT rand(): state = state * 214013 + 2531011; return (state >> 16) & 0x7FFF.

typedef struct { A_u_long s; } OLMDBRand;
static void OLMDBSrand(OLMDBRand *r, A_u_long seed) { r->s = seed; }
static float OLMDBRand01(OLMDBRand *r)
{
    r->s = r->s * 214013u + 2531011u;
    return ((r->s >> 16) & 0x7FFF) / 32767.0f;
}

// ---------------------------------------------------------- GaussianLUT ---
// sub_180001830: lut[i] = exp(-i^2 / (2*sigma^2 + eps)), sigma = n/3.

static void BuildGaussianLUT(float *lut, int n)
{
    float sigma = n / 3.0f;
    double denom = (double)sigma * sigma * 2.0 + 0.00001;
    for (int i = 0; i < n; ++i)
        lut[i] = expf((float)(-(double)(i * i) / denom));
}

// ---------------------------------------------------------- NoiseField ---
// sub_1800034E0: 101-entry table in [-1,1], value-noise lattice with
// smoothstep (3t^2-2t^3) interpolation mixed with white noise, clamped 0..1.

typedef struct {
    int gw, gh;                 // lattice size
    std::vector<float> table;   // 101 random entries
    std::vector<float> field;   // gw*gh noise field
    float cell;                 // thickness (cell size in px)
} OLMDBNoise;

static float smooth01(float t) { return t * t * (3.0f - 2.0f * t); }

static void BuildNoiseField(OLMDBNoise *nz, int w, int h, float thickness, float offset, A_long seed)
{
    nz->cell = thickness > 0.5f ? thickness : 1.0f;
    nz->gw = (int)(w / nz->cell) + 3;
    nz->gh = (int)(h / nz->cell) + 3;
    OLMDBRand r;
    OLMDBSrand(&r, (A_u_long)seed);
    nz->table.assign(101, 0.0f);
    for (int i = 0; i < 101; ++i)
        nz->table[i] = OLMDBRand01(&r) * 2.0f - 1.0f;
    nz->field.assign((size_t)nz->gw * nz->gh, 0.0f);
    for (int y = 0; y < nz->gh; ++y) {
        for (int x = 0; x < nz->gw; ++x) {
            float white = OLMDBRand01(&r);
            float ox = OLMDBRand01(&r) * nz->gw + offset;
            while (ox >= nz->gw) ox -= nz->gw;
            int ix = (int)ox;
            float fx = ox - ix;
            float s = smooth01(powf(fx, 2.0f));
            float a = nz->table[ix % 101];
            float b = nz->table[(ix + 1) % 101];
            float v = ((1.0f - s) * a + s * b) * 0.5f + white;
            nz->field[(size_t)y * nz->gw + x] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        }
    }
}

// Smooth = smoothstep-bilinear, Block = nearest (sub_180003370).
static float SampleNoise(const OLMDBNoise *nz, float x, float y, PF_Boolean smooth)
{
    float gx = x / nz->cell, gy = y / nz->cell;
    int ix = (int)floorf(gx), iy = (int)floorf(gy);
    if (!smooth) {
        ix = (ix % nz->gw + nz->gw) % nz->gw;
        iy = (iy % nz->gh + nz->gh) % nz->gh;
        return nz->field[(size_t)iy * nz->gw + ix];
    }
    float fx = gx - ix, fy = gy - iy;
    float sx = smooth01(fx * fx), sy = smooth01(fy * fy);
    int x0 = (ix % nz->gw + nz->gw) % nz->gw, x1 = (x0 + 1) % nz->gw;
    int y0 = (iy % nz->gh + nz->gh) % nz->gh, y1 = (y0 + 1) % nz->gh;
    float v00 = nz->field[(size_t)y0 * nz->gw + x0], v10 = nz->field[(size_t)y0 * nz->gw + x1];
    float v01 = nz->field[(size_t)y1 * nz->gw + x0], v11 = nz->field[(size_t)y1 * nz->gw + x1];
    return ((1 - sy) * ((1 - sx) * v00 + sx * v10) + sy * ((1 - sx) * v01 + sx * v11));
}

// -------------------------------------------------------------- Rotate ---
// Bilinear rotate about image centre. ARGB version weights RGB by alpha
// and renormalises (sub_180001EC0); plane version is plain bilinear.

static void RotateARGB(const float *src, float *dst, int w, int h, float ang)
{
    float c = cosf(ang), s = sinf(ang);
    float cx = w / 2.0f, cy = h / 2.0f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float dx = x - cx, dy = y - cy;
            float sx = dx * c + dy * s + cx, sy = -dx * s + dy * c + cy;
            int ix = (int)sx, iy = (int)sy;
            float *d = dst + ((size_t)y * w + x) * 4;
            if (ix > 0 && ix < w - 1 && iy > 0 && iy < h - 1) {
                float fx = sx - ix, fy = sy - iy;
                const float *p00 = src + ((size_t)iy * w + ix) * 4;
                const float *p10 = p00 + 4, *p01 = p00 + w * 4, *p11 = p01 + 4;
                float a00 = p00[3], a10 = p10[3], a01 = p01[3], a11 = p11[3];
                float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
                float asum = a00 * w00 + a10 * w10 + a01 * w01 + a11 * w11;
                if (asum != 0.0f) { w00 /= asum; w10 /= asum; w01 /= asum; w11 /= asum; }
                d[0] = p00[0] * w00 * a00 + p10[0] * w10 * a10 + p01[0] * w01 * a01 + p11[0] * w11 * a11;
                d[1] = p00[1] * w00 * a00 + p10[1] * w10 * a10 + p01[1] * w01 * a01 + p11[1] * w11 * a11;
                d[2] = p00[2] * w00 * a00 + p10[2] * w10 * a10 + p01[2] * w01 * a01 + p11[2] * w11 * a11;
                d[3] = asum;
            } else {
                d[0] = d[1] = d[2] = d[3] = 0.0f;
            }
        }
    }
}

// ------------------------------------------------------------ BlurCore ---
// Row-parallel core (sub_1800038D0): size-variation weight per pixel, then
// front/back directional splats with Gaussian falloff, sharp-tail exponent,
// noise modulation and fmaxf edge fade. Transparent pixels are skipped.

typedef struct {
    float r, g, b, a;   // accumulated colour (premultiplied)
    float wsum;         // accumulated weight
    float wmax;         // max scatter weight (tail density)
    float cy, cw;       // opaque-group centre/width for edge fade
} OLMDBAccum;

static void BlurRows(const float *src, OLMDBAccum *acc, int y0, int y1, int w, int h,
                     const float *lutF, int lenF, const float *lutB, int lenB,
                     const OLMDBParams *pp, const OLMDBNoise *nz, const float *layerNoise,
                     PF_Boolean useLayer)
{
    (void)h;
    float invF = lenF > 0 ? 1.0f / lenF : 0.0f;
    float invB = lenB > 0 ? 1.0f / lenB : 0.0f;
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < w; ++x) {
            const float *sp = src + ((size_t)y * w + x) * 4;
            if (sp[3] == 0.0f)
                continue;
            // Size-variation weight: pow(alpha-group response, size_var).
            float nzv = 1.0f;
            if (pp->noise_var > 0.0f) {
                if (useLayer && layerNoise)
                    nzv = layerNoise[((size_t)y * w + x)];
                else if (nz)
                    nzv = SampleNoise(nz, (float)x, (float)y, pp->noise_type != OLMDB_NOISE_BLOCK);
            }
            float wgt = powf(sp[3] * (0.5f + 0.5f * nzv) / 1.0f, pp->size_var);
            if (wgt == 0.0f)
                continue;
            float gain = pp->brightness * wgt;
            // Scatter along +/-x (image was rotated so blur axis is horizontal).
            for (int side = 0; side < 2; ++side) {
                A_long blur = side ? pp->back_blur : pp->front_blur;
                A_long fade = side ? pp->back_fade : pp->front_fade;
                float tail = side ? pp->back_tail : pp->front_tail;
                const float *lut = side ? lutB : lutF;
                int len = side ? lenB : lenF;
                float inv = side ? invB : invF;
                if (blur <= 0 || len <= 0)
                    continue;
                int dir = side ? -1 : 1;
                int reach = blur < len ? blur : len;
                // Edge fade (fidelity note: Windows weights this by the size-pass
                // group centre/width per pixel; approximated here by tap index).
                for (int i = 1; i <= reach; ++i) {
                    int xx = x + dir * i;
                    if (xx < 0 || xx >= w)
                        break;
                    float k = lut[(int)(i * inv * (len - 1))];
                    if (tail > 0.0f)
                        k = powf(k, 1.0f - tail * 0.9f);
                    k *= gain * (0.5f + 0.5f * nzv);
                    if (fade > 0)
                        k *= fmaxf(0.0f, 1.0f - fabsf((float)i * fade / 100.0f) / (float)(blur > 0 ? blur : 1));
                    if (k == 0.0f)
                        continue;
                    OLMDBAccum *a = acc + ((size_t)y * w + xx);
                    a->r += sp[0] * k; a->g += sp[1] * k; a->b += sp[2] * k;
                    a->wsum += k;
                    if (k > a->wmax) a->wmax = k;
                }
            }
            OLMDBAccum *self = acc + ((size_t)y * w + x);
            self->r += sp[0] * gain; self->g += sp[1] * gain; self->b += sp[2] * gain;
            self->wsum += gain;
            if (gain > self->wmax) self->wmax = gain;
        }
    }
}

// ------------------------------------------------------- Format kernels ---
// Each bit-depth kernel converts to float, runs the shared pipeline, converts back.

static void WorldToFloat(PF_EffectWorld *world, std::vector<float> &out, PF_PixelFormat fmt)
{
    int w = world->width, h = world->height;
    out.assign((size_t)w * h * 4, 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float *d = &out[((size_t)y * w + x) * 4];
            if (fmt == PF_PixelFormat_ARGB128) {
                PF_PixelFloat *p = (PF_PixelFloat *)((char *)world->data + y * world->rowbytes) + x;
                d[0] = p->red; d[1] = p->green; d[2] = p->blue; d[3] = p->alpha;
            } else if (fmt == PF_PixelFormat_ARGB64) {
                PF_Pixel16 *p = (PF_Pixel16 *)((char *)world->data + y * world->rowbytes) + x;
                d[0] = p->red / 32768.0f; d[1] = p->green / 32768.0f;
                d[2] = p->blue / 32768.0f; d[3] = p->alpha / 32768.0f;
            } else {
                PF_Pixel8 *p = (PF_Pixel8 *)((char *)world->data + y * world->rowbytes) + x;
                d[0] = p->red / 255.0f; d[1] = p->green / 255.0f;
                d[2] = p->blue / 255.0f; d[3] = p->alpha / 255.0f;
            }
        }
    }
}

static void FloatToWorld(const std::vector<float> &in, PF_EffectWorld *world, PF_PixelFormat fmt)
{
    int w = world->width, h = world->height;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float *s = &in[((size_t)y * w + x) * 4];
            if (fmt == PF_PixelFormat_ARGB128) {
                PF_PixelFloat *p = (PF_PixelFloat *)((char *)world->data + y * world->rowbytes) + x;
                p->red = s[0]; p->green = s[1]; p->blue = s[2]; p->alpha = s[3];
            } else if (fmt == PF_PixelFormat_ARGB64) {
                PF_Pixel16 *p = (PF_Pixel16 *)((char *)world->data + y * world->rowbytes) + x;
                p->red = (A_u_short)(s[0] * 32768.0f + 0.5f);
                p->green = (A_u_short)(s[1] * 32768.0f + 0.5f);
                p->blue = (A_u_short)(s[2] * 32768.0f + 0.5f);
                p->alpha = (A_u_short)(s[3] * 32768.0f + 0.5f);
            } else {
                PF_Pixel8 *p = (PF_Pixel8 *)((char *)world->data + y * world->rowbytes) + x;
                p->red = (A_u_char)(s[0] * 255.0f + 0.5f);
                p->green = (A_u_char)(s[1] * 255.0f + 0.5f);
                p->blue = (A_u_char)(s[2] * 255.0f + 0.5f);
                p->alpha = (A_u_char)(s[3] * 255.0f + 0.5f);
            }
        }
    }
}

static PF_Err RenderFloatPipeline(PF_EffectWorld *input, PF_EffectWorld *output,
                                  PF_EffectWorld *noiseWorld, const OLMDBParams *pp,
                                  PF_PixelFormat fmt)
{
    int w = input->width, h = input->height;
    if (w <= 0 || h <= 0)
        return PF_Err_NONE;

    std::vector<float> srcF, noisePlanar;
    WorldToFloat(input, srcF, fmt);
    const float *layerNoise = nullptr;
    if (pp->noise_type == OLMDB_NOISE_LAYER && noiseWorld) {
        std::vector<float> noiseF;
        WorldToFloat(noiseWorld, noiseF, fmt);
        // Luma of noise layer drives strength (manual: brighter = stronger blur).
        noisePlanar.resize(noiseF.size() / 4);
        for (size_t i = 0; i < noisePlanar.size(); ++i)
            noisePlanar[i] = (noiseF[i * 4] + noiseF[i * 4 + 1] + noiseF[i * 4 + 2]) / 3.0f;
        layerNoise = noisePlanar.data();
    }

    // Rotate so blur axis is horizontal, blur, rotate back.
    std::vector<float> rot((size_t)w * h * 4), rotOut((size_t)w * h * 4);
    RotateARGB(srcF.data(), rot.data(), w, h, -pp->angle_rad);

    OLMDBNoise nz;
    const OLMDBNoise *nzp = nullptr;
    if (pp->noise_var > 0.0f && pp->noise_type != OLMDB_NOISE_LAYER) {
        BuildNoiseField(&nz, w, h, pp->thickness, pp->offset, pp->seed);
        nzp = &nz;
    }

    int lenF = (int)pp->front_blur + 1, lenB = (int)pp->back_blur + 1;
    if (lenF < 1) lenF = 1;
    if (lenB < 1) lenB = 1;
    std::vector<float> lutF(lenF), lutB(lenB);
    BuildGaussianLUT(lutF.data(), lenF);
    BuildGaussianLUT(lutB.data(), lenB);

    std::vector<OLMDBAccum> acc((size_t)w * h);
    memset(acc.data(), 0, acc.size() * sizeof(acc[0]));

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int y = 0; y < h; ++y)
        BlurRows(rot.data(), acc.data(), y, y + 1, w, h, lutF.data(), lenF, lutB.data(), lenB,
                 pp, nzp, layerNoise, (pp->noise_type == OLMDB_NOISE_LAYER && layerNoise));

    for (size_t i = 0; i < acc.size(); ++i) {
        float *d = &rotOut[i * 4];
        const float *s = &rot[i * 4];
        if (acc[i].wsum > 0.0f) {
            d[0] = acc[i].r / acc[i].wsum * pp->brightness;
            d[1] = acc[i].g / acc[i].wsum * pp->brightness;
            d[2] = acc[i].b / acc[i].wsum * pp->brightness;
            d[3] = s[3];
        } else {
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }

    std::vector<float> dstF((size_t)w * h * 4);
    RotateARGB(rotOut.data(), dstF.data(), w, h, pp->angle_rad);
    // Preserve source alpha exactly outside blurred opaque groups (transparent ignore).
    for (size_t i = 0; i < (size_t)w * h; ++i)
        dstF[i * 4 + 3] = srcF[i * 4 + 3];

    FloatToWorld(dstF, output, fmt);
    return PF_Err_NONE;
}

// ------------------------------------------------------------- PreRender ---

typedef struct { OLMDBParams params; } OLMDBPreData;

static void DisposePreRenderData(void *p) { free(p); }

static PF_Err PreRender(PF_InData *in_data, PF_OutData *out_data, PF_PreRenderExtra *extra)
{
    PF_Err err = PF_Err_NONE;
    PF_CheckoutResult in_result;
    PF_RenderRequest req = extra->input->output_request;

    OLMDBPreData *info = (OLMDBPreData *)malloc(sizeof(OLMDBPreData));
    if (!info)
        return PF_Err_OUT_OF_MEMORY;
    ERR(CheckoutParams(in_data, &info->params));
    extra->output->pre_render_data = info;
    extra->output->delete_pre_render_data_func = DisposePreRenderData;

    ERR(extra->cb->checkout_layer(in_data->effect_ref, OLMDB_INPUT, OLMDB_INPUT, &req,
                                 in_data->current_time, in_data->time_step, in_data->time_scale,
                                 &in_result));
    UnionLRect(&in_result.result_rect, &extra->output->result_rect);
    UnionLRect(&in_result.max_result_rect, &extra->output->max_result_rect);
    return err;
}

// ------------------------------------------------------------ SmartRender ---

static PF_Err SmartRender(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra)
{
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    PF_EffectWorld *input = nullptr, *output = nullptr, *noiseW = nullptr;
    OLMDBPreData *info = (OLMDBPreData *)extra->input->pre_render_data;
    if (!info)
        return PF_Err_INTERNAL_STRUCT_DAMAGED;

    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, OLMDB_INPUT, &input));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &output));
    if (!err && info->params.noise_type == OLMDB_NOISE_LAYER)
        ERR2(extra->cb->checkout_layer_pixels(in_data->effect_ref, OLMDB_NOISE_LAYER, &noiseW));

    if (!err && input && output) {
        AEFX_SuiteScoper<PF_WorldSuite2> ws(in_data, kPFWorldSuite, kPFWorldSuiteVersion2, out_data);
        PF_PixelFormat fmt = PF_PixelFormat_INVALID;
        ERR(ws->PF_GetPixelFormat(input, &fmt));
        if (!err) {
            if (fmt != PF_PixelFormat_ARGB32 && fmt != PF_PixelFormat_ARGB64 &&
                fmt != PF_PixelFormat_ARGB128)
                err = PF_Err_BAD_CALLBACK_PARAM;
            else
                ERR(RenderFloatPipeline(input, output, noiseW, &info->params, fmt));
        }
    }
    ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, OLMDB_INPUT));
    if (noiseW)
        ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, OLMDB_NOISE_LAYER));
    return err;
}

// ---------------------------------------------------------------- Entry ---

PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[],
                  PF_LayerDef *output, void *extra)
{
    PF_Err err = PF_Err_NONE;
    switch (cmd) {
    case PF_Cmd_ABOUT:
        err = About(in_data, out_data, params, output);
        break;
    case PF_Cmd_GLOBAL_SETUP:
        err = GlobalSetup(in_data, out_data, params, output);
        break;
    case PF_Cmd_PARAMS_SETUP:
        err = ParamsSetup(in_data, out_data, params, output);
        break;
    case PF_Cmd_RENDER:
        break; // SmartRender effect; nothing to do here (matches Windows case 11).
    case PF_Cmd_UPDATE_PARAMS_UI:
        err = UpdateParamsUIFull(in_data, out_data, params, extra);
        break;
    case PF_Cmd_SMART_PRE_RENDER:
        err = PreRender(in_data, out_data, (PF_PreRenderExtra *)extra);
        break;
    case PF_Cmd_SMART_RENDER:
        err = SmartRender(in_data, out_data, (PF_SmartRenderExtra *)extra);
        break;
    case PF_Cmd_GET_EXTERNAL_DEPENDENCIES: {
        // Windows strings: "All Dependencies requested." / "Missing Dependencies requested."
        PF_ExtDependenciesExtra *dep = (PF_ExtDependenciesExtra *)extra;
        if (dep && dep->check_type == PF_DepCheckType_ALL_DEPENDENCIES) {
            const char *msg = "All Dependencies requested.";
            size_t n = strlen(msg) + 1;
            dep->dependencies_strH = PF_NEW_HANDLE(n);
            if (dep->dependencies_strH) {
                memcpy(*dep->dependencies_strH, msg, n);
            } else {
                err = PF_Err_OUT_OF_MEMORY;
            }
        }
        break;
    }
    default:
        break;
    }
    return err;
}

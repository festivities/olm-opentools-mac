/* OLM Toon Dilate: macOS port of OLMToonDilate.aex (entry 0x1801ABCA0).
 * Smart-render only. Dilates alpha==full pixels by a Chebyshev radius,
 * copying ARGB from the scan-order parent. No OpenCV calls in the plug-in. */

#include "OLMToonDilate.h"

#include <cstdint>
#include <vector>

namespace {

static_assert(PF_VERSION(OLMTD_MAJOR_VERSION, OLMTD_MINOR_VERSION, OLMTD_BUG_VERSION,
                         PF_Stage_DEVELOP, 0) == 559104,
              "Toon Dilate version must match its PiPL");

constexpr PF_OutFlags kOutFlags =
    PF_OutFlag_NON_PARAM_VARY | PF_OutFlag_USE_OUTPUT_EXTENT | PF_OutFlag_DEEP_COLOR_AWARE;
constexpr PF_OutFlags2 kOutFlags2 =
    PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |
    PF_OutFlag2_AUTOMATIC_WIDE_TIME_INPUT | PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
static_assert(kOutFlags == 0x02000044, "out_flags");
static_assert(kOutFlags2 == 0x08021400, "out_flags2");

AEGP_PluginID g_aegp_id = 0;

PF_Err About(PF_InData *in_data, PF_OutData *out_data) {
    PF_SPRINTF(out_data->return_msg, "%s", OLMTD_ABOUT);
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData *in_data, PF_OutData *out_data) {
    out_data->my_version = PF_VERSION(OLMTD_MAJOR_VERSION, OLMTD_MINOR_VERSION, OLMTD_BUG_VERSION,
                                      PF_Stage_DEVELOP, 0);
    out_data->out_flags = kOutFlags;
    out_data->out_flags2 = kOutFlags2;
    if (in_data->pica_basicP) {
        AEGP_SuiteHandler suites(in_data->pica_basicP);
        suites.UtilitySuite3()->AEGP_RegisterWithAEGP(nullptr, OLMTD_NAME, &g_aegp_id);
    }
    return PF_Err_NONE;
}

PF_Err ParamsSetup(PF_InData *in_data, PF_OutData *out_data) {
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDER("Search Radius", 0, 100, 0, 100, 0, 2.0, 1, PF_ValueDisplayFlag_NONE,
                        false, OLMTD_RADIUS);
    out_data->num_params = OLMTD_NUM_PARAMS;
    return PF_Err_NONE;
}

PF_Err PreRender(PF_InData *in_data, PF_PreRenderExtra *extra) {
    PF_RenderRequest req = extra->input->output_request;
    PF_CheckoutResult result;
    AEFX_CLR_STRUCT(result);
    PF_Err err = extra->cb->checkout_layer(in_data->effect_ref, OLMTD_INPUT, 0, &req,
                                           in_data->current_time, in_data->time_step,
                                           in_data->time_scale, &result);
    if (err) return err;
    UnionLRect(&result.result_rect, &extra->output->result_rect);
    UnionLRect(&result.max_result_rect, &extra->output->max_result_rect);
    extra->output->flags = PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS;
    return PF_Err_NONE;
}

// float32 ceil of (downsample_x.num/den) * slider. downsample_y is ignored.
float ScaledRadius(const PF_InData *in_data, float radius) {
    float r = (float)in_data->downsample_x.num / (float)in_data->downsample_x.den * radius;
    const int t = (int)r;
    if (t != (int)0x80000000 && (float)t != r) r = (float)(t + (r >= 0.f ? 1 : 0));
    return r;
}

template <typename Pix>
Pix *At(PF_EffectWorld *w, int x, int y) {
    return (Pix *)((char *)w->data + (size_t)y * w->rowbytes) + x;
}

template <typename Pix>
bool In(const PF_EffectWorld *w, int x, int y) {
    return x >= 0 && y >= 0 && x < w->width && y < w->height;
}

// nxy: dx,dy. Forward: L, NW, N, NE. Backward: R, SE, S, SW. Strict-less wins.
template <typename Pix, typename Seed>
void Dilate(const PF_EffectWorld *src, PF_EffectWorld *dst, float radius, Seed seed) {
    const int w = src->width, h = src->height;
    if (w <= 0 || h <= 0) return;
    std::vector<std::uint32_t> mask((size_t)w * h, 0xFFFFFFFFu);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (seed(*At<Pix>(const_cast<PF_EffectWorld *>(src), x, y)))
                mask[(size_t)y * w + x] = 0;

    auto sample = [&](int x, int y) -> std::uint32_t {
        if (x < 0 || y < 0 || x >= w || y >= h) return 0xFFFFFFFFu;
        return mask[(size_t)y * w + x];
    };
    auto relax = [&](int x, int y, const int nxy[4][2]) {
        const size_t i = (size_t)y * w + x;
        if (mask[i] == 0) return;
        std::uint32_t best = 0xFFFFFFFFu;
        int bi = 0;
        for (int k = 0; k < 4; ++k) {
            const std::uint32_t v = sample(x + nxy[k][0], y + nxy[k][1]);
            if (v < best) {
                best = v;
                bi = k;
            }
        }
        if (best == 0xFFFFFFFFu) return;
        const std::uint32_t d = best + 1;
        if (mask[i] > d) {
            mask[i] = d;
            if ((float)d <= radius) {
                const int nx = x + nxy[bi][0], ny = y + nxy[bi][1];
                if (In<Pix>(dst, nx, ny) && In<Pix>(dst, x, y))
                    *At<Pix>(dst, x, y) = *At<Pix>(dst, nx, ny);
            }
        }
    };
    const int fwd[4][2] = {{-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    const int bwd[4][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) relax(x, y, fwd);
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) relax(x, y, bwd);
}

template <typename Pix, typename Seed>
PF_Err Render(PF_InData *in_data, PF_EffectWorld *input, PF_EffectWorld *output, float radius,
              Seed seed) {
    PF_Err err = in_data->utils->copy(in_data->effect_ref, input, output, nullptr, nullptr);
    if (!err) Dilate<Pix>(input, output, ScaledRadius(in_data, radius), seed);
    return err;
}

PF_Err SmartRender(PF_InData *in_data, PF_SmartRenderExtra *extra) {
    const int depth = extra->input->bitdepth;
    if (depth != 8 && depth != 16 && depth != 32) return PF_Err_NONE;
    PF_EffectWorld *input = nullptr;
    PF_EffectWorld *output = nullptr;
    PF_Err err = extra->cb->checkout_layer_pixels(in_data->effect_ref, OLMTD_INPUT, &input);
    if (!err) err = extra->cb->checkout_output(in_data->effect_ref, &output);
    if (!err && (!input || !output)) err = PF_Err_BAD_CALLBACK_PARAM;
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    if (!err)
        err = PF_CHECKOUT_PARAM(in_data, OLMTD_RADIUS, in_data->current_time, in_data->time_step,
                                in_data->time_scale, &def);
    if (!err) {
        const float radius = (float)def.u.fs_d.value;
        if (depth == 8)
            err = Render<PF_Pixel>(in_data, input, output, radius,
                                   [](const PF_Pixel &p) { return p.alpha == 255; });
        else if (depth == 16)
            err = Render<PF_Pixel16>(in_data, input, output, radius,
                                     [](const PF_Pixel16 &p) { return p.alpha == 32768; });
        else
            err = Render<PF_PixelFloat>(in_data, input, output, radius,
                                        [](const PF_PixelFloat &p) { return p.alpha == 1.0f; });
        const PF_Err cin = PF_CHECKIN_PARAM(in_data, &def);
        if (!err) err = cin;
    }
    return err;
}

}  // namespace

PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[],
                  PF_LayerDef *output, void *extra) {
    (void)params;
    (void)output;
    switch (cmd) {
        case PF_Cmd_ABOUT: return About(in_data, out_data);
        case PF_Cmd_GLOBAL_SETUP: return GlobalSetup(in_data, out_data);
        case PF_Cmd_PARAMS_SETUP: return ParamsSetup(in_data, out_data);
        case PF_Cmd_SMART_PRE_RENDER: return PreRender(in_data, (PF_PreRenderExtra *)extra);
        case PF_Cmd_SMART_RENDER: return SmartRender(in_data, (PF_SmartRenderExtra *)extra);
        default: return PF_Err_NONE;
    }
}

// Fake only the AE host callbacks; the effect entry, parameter setup/checkout,
// SmartRender, iterate callbacks, render core, and pixel writers are production code.
#include "OLMDirectionalBlur.cpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr A_long kWidth = 17;
constexpr A_long kHeight = 9;
constexpr int kBlur = 8;
constexpr int kFade = 8;

struct TestHost {
    PF_WorldSuite2 world_suite{};
    PF_Iterate8Suite1 iterate8{};
    PF_Iterate16Suite1 iterate16{};
    PF_IterateFloatSuite1 iterate_float{};
    SPBasicSuite basic{};
    PF_UtilCallbacks utils{};
    PF_InData in{};
    PF_OutData out{};
    PF_EffectWorld input{};
    PF_EffectWorld output{};
    PF_PreRenderInput pre_render_input{};
    PF_PreRenderOutput pre_render_output{};
    PF_PreRenderCallbacks pre_render_callbacks{};
    PF_PreRenderExtra pre_render_extra{};
    PF_SmartRenderInput render_input{};
    PF_SmartRenderCallbacks render_callbacks{};
    PF_SmartRenderExtra render_extra{};
    PF_PixelFormat format = PF_PixelFormat_INVALID;
    std::array<PF_ParamDef, OLMDB_NUM_PARAMS> params{};
    std::array<bool, OLMDB_NUM_PARAMS> param_added{};
    std::array<unsigned, OLMDB_NUM_PARAMS> checkouts{};
    std::array<unsigned, OLMDB_NUM_PARAMS> checkins{};
    bool params_setup_ok = false;
    unsigned layer_checkouts = 0;
    unsigned layer_checkins = 0;
    unsigned pre_render_checkouts = 0;
    A_long width = kWidth;
    A_long height = kHeight;
    A_long alpha_left = 3;
    A_long alpha_top = 2;
    A_long alpha_right = 14;
    A_long alpha_bottom = 7;
    bool opaque_plateau = false;
    std::vector<A_u_char> input_pixels;
    std::vector<A_u_char> output_pixels;
};

TestHost *g_host = nullptr;

PF_Err add_param(PF_ProgPtr, PF_ParamIndex, PF_ParamDefPtr def) {
    const A_long id = def->uu.id;
    if (!g_host || id < 0 || id >= OLMDB_NUM_PARAMS)
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    g_host->params[(size_t)id] = *def;
    g_host->param_added[(size_t)id] = true;
    return PF_Err_NONE;
}

PF_Err checkout_param(PF_ProgPtr, PF_ParamIndex id, A_long, A_long, A_u_long,
                      PF_ParamDef *param) {
    if (!g_host || id <= OLMDB_INPUT || id >= OLMDB_NUM_PARAMS ||
        !g_host->param_added[(size_t)id])
        return PF_Err_BAD_CALLBACK_PARAM;
    ++g_host->checkouts[(size_t)id];
    *param = g_host->params[(size_t)id];
    switch (id) {
    case OLMDB_ANGLE: param->u.ad.value = FLOAT2FIX(0); break;
    case OLMDB_BRIGHTNESS: param->u.fs_d.value = 1.0; break;
    case OLMDB_FRONT_BLUR: param->u.sd.value = kBlur; break;
    case OLMDB_FRONT_TAIL: param->u.fd.value = FLOAT2FIX(0); break;
    case OLMDB_BACK_BLUR: param->u.sd.value = kBlur; break;
    case OLMDB_BACK_TAIL: param->u.fd.value = FLOAT2FIX(0); break;
    case OLMDB_SIZE_VAR: param->u.fd.value = FLOAT2FIX(0); break;
    case OLMDB_NOISE_VAR: param->u.fd.value = FLOAT2FIX(0); break;
    case OLMDB_NOISE_TYPE: param->u.pd.value = OLMDB_POPUP_SMOOTH; break;
    case OLMDB_SEED: param->u.sd.value = 1; break;
    case OLMDB_OFFSET: param->u.ad.value = FLOAT2FIX(0); break;
    case OLMDB_THICKNESS: param->u.fs_d.value = 10.0; break;
    default: break;
    }
    return PF_Err_NONE;
}

PF_Err checkin_param(PF_ProgPtr, PF_ParamDef *param) {
    if (!g_host || param->uu.id <= OLMDB_INPUT || param->uu.id >= OLMDB_NUM_PARAMS)
        return PF_Err_BAD_CALLBACK_PARAM;
    ++g_host->checkins[(size_t)param->uu.id];
    return PF_Err_NONE;
}

PF_Err checkin_layer_pixels(PF_ProgPtr, A_long) {
    if (g_host) ++g_host->layer_checkins;
    return PF_Err_NONE;
}

PF_Err checkout_layer_pixels(PF_ProgPtr, A_long, PF_EffectWorld **pixels) {
    if (!g_host || !pixels) return PF_Err_BAD_CALLBACK_PARAM;
    ++g_host->layer_checkouts;
    *pixels = &g_host->input;
    return PF_Err_NONE;
}

PF_Err pre_render_checkout_layer(PF_ProgPtr, PF_ParamIndex index, A_long checkout_id,
                                 const PF_RenderRequest *, A_long, A_long, A_u_long,
                                 PF_CheckoutResult *result) {
    if (!g_host || !result || index != OLMDB_INPUT || checkout_id != 0)
        return PF_Err_BAD_CALLBACK_PARAM;
    ++g_host->pre_render_checkouts;
    result->result_rect = g_host->input.extent_hint;
    result->max_result_rect = g_host->input.extent_hint;
    result->ref_width = g_host->in.width;
    result->ref_height = g_host->in.height;
    return PF_Err_NONE;
}

PF_Err checkout_output(PF_ProgPtr, PF_EffectWorld **pixels) {
    if (!g_host || !pixels) return PF_Err_BAD_CALLBACK_PARAM;
    *pixels = &g_host->output;
    return PF_Err_NONE;
}

PF_Err get_pixel_format(const PF_EffectWorld *, PF_PixelFormat *format) {
    if (!g_host || !format) return PF_Err_BAD_CALLBACK_PARAM;
    *format = g_host->format;
    return PF_Err_NONE;
}

PF_Err copy_world(PF_ProgPtr, PF_EffectWorld *src, PF_EffectWorld *dst,
                  PF_Rect *, PF_Rect *) {
    if (!src || !dst || !src->data || !dst->data || src->rowbytes != dst->rowbytes ||
        src->height != dst->height)
        return PF_Err_BAD_CALLBACK_PARAM;
    std::memcpy(dst->data, src->data, (size_t)src->rowbytes * (size_t)src->height);
    return PF_Err_NONE;
}

template <typename Pixel, typename PixelFn>
PF_Err iterate_pixels(PF_EffectWorld *src, const PF_Rect *area, void *refcon,
                      PixelFn pixel_fn, PF_EffectWorld *dst) {
    if (!src || !dst || !src->data || !dst->data || !area || !pixel_fn)
        return PF_Err_BAD_CALLBACK_PARAM;
    for (A_long y = area->top; y < area->bottom; ++y) {
        for (A_long x = area->left; x < area->right; ++x) {
            auto *in = reinterpret_cast<Pixel *>(reinterpret_cast<char *>(src->data) +
                                                  (size_t)y * (size_t)src->rowbytes) + x;
            auto *out = reinterpret_cast<Pixel *>(reinterpret_cast<char *>(dst->data) +
                                                   (size_t)y * (size_t)dst->rowbytes) + x;
            PF_Err err = pixel_fn(refcon, x, y, in, out);
            if (err) return err;
        }
    }
    return PF_Err_NONE;
}

PF_Err iterate8(PF_InData *, A_long, A_long, PF_EffectWorld *src, const PF_Rect *area,
                void *refcon, PF_Err (*pixel_fn)(void *, A_long, A_long, PF_Pixel *, PF_Pixel *),
                PF_EffectWorld *dst) {
    return iterate_pixels<PF_Pixel>(src, area, refcon, pixel_fn, dst);
}

PF_Err iterate16(PF_InData *, A_long, A_long, PF_EffectWorld *src, const PF_Rect *area,
                 void *refcon, PF_Err (*pixel_fn)(void *, A_long, A_long, PF_Pixel16 *, PF_Pixel16 *),
                 PF_EffectWorld *dst) {
    return iterate_pixels<PF_Pixel16>(src, area, refcon, pixel_fn, dst);
}

PF_Err iterate_float(PF_InData *, A_long, A_long, PF_EffectWorld *src, const PF_Rect *area,
                     void *refcon,
                     PF_Err (*pixel_fn)(void *, A_long, A_long, PF_PixelFloat *, PF_PixelFloat *),
                     PF_EffectWorld *dst) {
    return iterate_pixels<PF_PixelFloat>(src, area, refcon, pixel_fn, dst);
}

PF_Err SPAPI acquire_suite(const char *name, int32 version, const void **suite) {
    if (!g_host || !name || !suite) return (SPErr)-1;
    if (!std::strcmp(name, kPFWorldSuite) && version == kPFWorldSuiteVersion2)
        *suite = &g_host->world_suite;
    else if (!std::strcmp(name, kPFIterate8Suite) && version == kPFIterate8SuiteVersion1)
        *suite = &g_host->iterate8;
    else if (!std::strcmp(name, kPFIterate16Suite) && version == kPFIterate16SuiteVersion1)
        *suite = &g_host->iterate16;
    else if (!std::strcmp(name, kPFIterateFloatSuite) && version == kPFIterateFloatSuiteVersion1)
        *suite = &g_host->iterate_float;
    else
        return (SPErr)-1;
    return kSPNoError;
}

SPErr SPAPI release_suite(const char *, int32) { return kSPNoError; }

void set_world(PF_EffectWorld &world, std::vector<A_u_char> &pixels, A_long width,
               A_long height, A_long rowbytes) {
    world.data = reinterpret_cast<PF_PixelPtr>(pixels.data());
    world.rowbytes = rowbytes;
    world.width = width;
    world.height = height;
    world.extent_hint.left = 0;
    world.extent_hint.top = 0;
    world.extent_hint.right = width;
    world.extent_hint.bottom = height;
}

float source_alpha(const TestHost &host, int x, int y) {
    if (x < host.alpha_left || x >= host.alpha_right ||
        y < host.alpha_top || y >= host.alpha_bottom)
        return 0.0f;
    if (host.opaque_plateau) return 1.0f;
    const int pattern = (x * 3 + y * 5) % 7;
    return 0.18f + 0.82f * (float)pattern / 6.0f;
}

template <typename Pixel, typename Channel>
void fill_input(const TestHost &host, std::vector<A_u_char> &bytes,
                A_long rowbytes, float channel_scale) {
    for (A_long y = 0; y < host.height; ++y) {
        for (A_long x = 0; x < host.width; ++x) {
            auto *p = reinterpret_cast<Pixel *>(bytes.data() + (size_t)y * (size_t)rowbytes) + x;
            const float alpha = source_alpha(host, x, y);
            const float red = alpha * (0.12f + 0.7f * (float)x / (float)host.width);
            const float green = alpha * (0.15f + 0.6f * (float)y / (float)host.height);
            const float blue = alpha * (0.7f - 0.4f * (float)x / (float)host.width);
            p->alpha = (Channel)std::lround(alpha * channel_scale);
            p->red = (Channel)std::lround(red * channel_scale);
            p->green = (Channel)std::lround(green * channel_scale);
            p->blue = (Channel)std::lround(blue * channel_scale);
        }
    }
}

void initialize_host(TestHost &host, short bitdepth, int fade, A_long downsample_den,
                     A_long width, A_long height, bool opaque_plateau) {
    g_host = &host;
    host.width = width;
    host.height = height;
    host.opaque_plateau = opaque_plateau;
    if (opaque_plateau) {
        host.alpha_left = 40;
        host.alpha_top = 40;
        host.alpha_right = 121;
        host.alpha_bottom = 121;
    }
    host.format = bitdepth == 8 ? PF_PixelFormat_ARGB32
                 : bitdepth == 16 ? PF_PixelFormat_ARGB64
                                  : PF_PixelFormat_ARGB128;
    const A_long bytes_per_pixel = bitdepth == 8 ? sizeof(PF_Pixel)
                                     : bitdepth == 16 ? sizeof(PF_Pixel16)
                                                      : sizeof(PF_PixelFloat);
    const A_long rowbytes = host.width * bytes_per_pixel;
    host.input_pixels.assign((size_t)rowbytes * (size_t)host.height, 0);
    host.output_pixels.assign((size_t)rowbytes * (size_t)host.height, 0);
    if (bitdepth == 8)
        fill_input<PF_Pixel, A_u_char>(host, host.input_pixels, rowbytes, 255.0f);
    else if (bitdepth == 16)
        fill_input<PF_Pixel16, A_u_short>(host, host.input_pixels, rowbytes, 32768.0f);
    else
        fill_input<PF_PixelFloat, PF_FpShort>(host, host.input_pixels, rowbytes, 1.0f);

    set_world(host.input, host.input_pixels, host.width, host.height, rowbytes);
    set_world(host.output, host.output_pixels, host.width, host.height, rowbytes);
    host.input.extent_hint.left = host.alpha_left;
    host.input.extent_hint.top = host.alpha_top;
    host.input.extent_hint.right = host.alpha_right;
    host.input.extent_hint.bottom = host.alpha_bottom;

    host.world_suite.PF_GetPixelFormat = get_pixel_format;
    host.iterate8.iterate = iterate8;
    host.iterate16.iterate = iterate16;
    host.iterate_float.iterate = iterate_float;
    host.basic.AcquireSuite = acquire_suite;
    host.basic.ReleaseSuite = release_suite;
    host.utils.copy = copy_world;

    host.in.inter.add_param = add_param;
    host.in.inter.checkout_param = checkout_param;
    host.in.inter.checkin_param = checkin_param;
    host.in.utils = &host.utils;
    host.in.effect_ref = reinterpret_cast<PF_ProgPtr>(&host);
    host.in.pica_basicP = &host.basic;
    host.in.current_time = 0;
    host.in.time_step = 1;
    host.in.time_scale = 30;
    host.in.width = host.width * downsample_den;
    host.in.height = host.height * downsample_den;
    host.in.downsample_x.num = 1;
    host.in.downsample_x.den = downsample_den;
    host.in.extent_hint = host.input.extent_hint;

    if (EffectMain(PF_Cmd_PARAMS_SETUP, &host.in, &host.out, nullptr, nullptr, nullptr) != PF_Err_NONE ||
        host.out.num_params != OLMDB_NUM_PARAMS)
        return;
    for (PF_ParamIndex id : {OLMDB_FRONT_FADE, OLMDB_BACK_FADE}) {
        const PF_ParamDef &def = host.params[(size_t)id];
        if (def.param_type != PF_Param_SLIDER || def.u.sd.valid_min != 0 ||
            def.u.sd.valid_max != 100 || def.u.sd.slider_min != 0 ||
            def.u.sd.slider_max != 100 || def.u.sd.dephault != 0 || def.u.sd.value != 0)
            return;
    }
    host.params_setup_ok = true;

    // The fake checkout callback starts from the actual PARAMS_SETUP definitions.
    host.params[OLMDB_FRONT_FADE].u.sd.value = fade;
    host.params[OLMDB_BACK_FADE].u.sd.value = fade;
    if (opaque_plateau) {
        host.pre_render_input.output_request.rect.left = 0;
        host.pre_render_input.output_request.rect.top = 0;
        host.pre_render_input.output_request.rect.right = host.width;
        host.pre_render_input.output_request.rect.bottom = host.height;
    } else {
        host.pre_render_input.output_request.rect.left = host.alpha_left + 2;
        host.pre_render_input.output_request.rect.top = host.alpha_top + 1;
        host.pre_render_input.output_request.rect.right = host.alpha_right - 2;
        host.pre_render_input.output_request.rect.bottom = host.alpha_bottom - 1;
    }
    host.pre_render_input.bitdepth = bitdepth;
    host.pre_render_output.result_rect = host.pre_render_input.output_request.rect;
    host.pre_render_output.max_result_rect = host.pre_render_input.output_request.rect;
    host.pre_render_callbacks.checkout_layer = pre_render_checkout_layer;
    host.pre_render_extra.input = &host.pre_render_input;
    host.pre_render_extra.output = &host.pre_render_output;
    host.pre_render_extra.cb = &host.pre_render_callbacks;
    // SmartRender uses these callback entries instead of the direct PF_Cmd_RENDER worlds.
    host.render_callbacks.checkout_layer_pixels = checkout_layer_pixels;
    host.render_callbacks.checkin_layer_pixels = checkin_layer_pixels;
    host.render_callbacks.checkout_output = checkout_output;
    host.render_input.output_request = host.pre_render_input.output_request;
    host.render_input.bitdepth = bitdepth;
    host.render_extra.input = &host.render_input;
    host.render_extra.cb = &host.render_callbacks;
}

struct DiffStats {
    unsigned changed_pixels = 0;
    unsigned changed_alpha = 0;
    unsigned changed_rgb = 0;
};

bool channel_changed(const std::vector<A_u_char> &a, const std::vector<A_u_char> &b,
                     size_t pixel, size_t channel, short bitdepth) {
    const size_t i = pixel * (bitdepth == 8 ? sizeof(PF_Pixel)
                           : bitdepth == 16 ? sizeof(PF_Pixel16)
                                            : sizeof(PF_PixelFloat));
    if (bitdepth == 8) return a[i + channel] != b[i + channel];
    if (bitdepth == 16) {
        A_u_short av, bv;
        std::memcpy(&av, &a[i + channel * 2], sizeof(av));
        std::memcpy(&bv, &b[i + channel * 2], sizeof(bv));
        return av != bv;
    }
    PF_FpShort av, bv;
    std::memcpy(&av, &a[i + channel * 4], sizeof(av));
    std::memcpy(&bv, &b[i + channel * 4], sizeof(bv));
    return std::fabs((float)(av - bv)) > 1e-7f;
}

bool pixel_changed(const std::vector<A_u_char> &a, const std::vector<A_u_char> &b,
                   size_t pixel, short bitdepth) {
    for (size_t channel = 0; channel < 4; ++channel)
        if (channel_changed(a, b, pixel, channel, bitdepth)) return true;
    return false;
}

DiffStats diff_outputs(const std::vector<A_u_char> &a, const std::vector<A_u_char> &b,
                       short bitdepth) {
    DiffStats stats;
    const size_t pixel_size = bitdepth == 8 ? sizeof(PF_Pixel)
                            : bitdepth == 16 ? sizeof(PF_Pixel16)
                                             : sizeof(PF_PixelFloat);
    for (size_t i = 0; i < a.size(); i += pixel_size) {
        bool pixel_changed = false;
        for (size_t c = 0; c < 4; ++c) {
            const bool changed = channel_changed(a, b, i / pixel_size, c, bitdepth);
            if (changed) {
                pixel_changed = true;
                if (c == 0) ++stats.changed_alpha;
                else ++stats.changed_rgb;
            }
        }
        if (pixel_changed) ++stats.changed_pixels;
    }
    return stats;
}

bool render(short bitdepth, int fade, std::vector<A_u_char> *pixels,
            A_long downsample_den = 1, A_long width = kWidth, A_long height = kHeight,
            bool opaque_plateau = false) {
    TestHost host;
    initialize_host(host, bitdepth, fade, downsample_den, width, height, opaque_plateau);
    if (!host.params_setup_ok || host.out.num_params != OLMDB_NUM_PARAMS) return false;
    PF_Err pre_err = EffectMain(PF_Cmd_SMART_PRE_RENDER, &host.in, &host.out, nullptr, nullptr,
                                &host.pre_render_extra);
    PF_LRect expected_result = host.pre_render_input.output_request.rect;
    PF_LRect expected_max = host.pre_render_input.output_request.rect;
    UnionLRect(&host.input.extent_hint, &expected_result);
    UnionLRect(&host.input.extent_hint, &expected_max);
    if (pre_err != PF_Err_NONE || host.pre_render_checkouts != 1 ||
        std::memcmp(&host.pre_render_output.result_rect, &expected_result, sizeof(expected_result)) ||
        std::memcmp(&host.pre_render_output.max_result_rect, &expected_max, sizeof(expected_max)))
        return false;
    host.output.extent_hint = host.pre_render_output.result_rect;
    PF_Err err = EffectMain(PF_Cmd_SMART_RENDER, &host.in, &host.out, nullptr, nullptr,
                            &host.render_extra);
    bool checkins_ok = true;
    for (PF_ParamIndex id = 1; id < OLMDB_NUM_PARAMS; ++id)
        checkins_ok &= host.checkouts[(size_t)id] == host.checkins[(size_t)id];
    const bool callbacks_ok = host.layer_checkouts == 1 && host.layer_checkins == 1;
    *pixels = std::move(host.output_pixels);
    g_host = nullptr;
    return err == PF_Err_NONE && callbacks_ok && checkins_ok;
}

}  // namespace

int main() {
    for (short bitdepth : {8, 16, 32}) {
        std::vector<A_u_char> no_fade, with_fade;
        if (!render(bitdepth, 0, &no_fade) || !render(bitdepth, kFade, &with_fade)) {
            std::fprintf(stderr, "%d-bit SmartRender plumbing failed\n", bitdepth);
            return 1;
        }
        const DiffStats diff = diff_outputs(no_fade, with_fade, bitdepth);
        std::printf("%d-bit blur+mixed-alpha: %u changed pixels, %u alpha channels, %u RGB channels\n",
                    bitdepth, diff.changed_pixels, diff.changed_alpha, diff.changed_rgb);
        if (!diff.changed_alpha || !diff.changed_rgb) {
            std::fprintf(stderr, "%d-bit Alpha Fade had no visible output delta\n", bitdepth);
            return 1;
        }

        std::vector<A_u_char> fade_one, quarter_no_fade, fade_four_at_quarter, quarter_fade8;
        if (!render(bitdepth, 1, &fade_one) ||
            !render(bitdepth, 0, &quarter_no_fade, 4) ||
            !render(bitdepth, 4, &fade_four_at_quarter, 4) ||
            !render(bitdepth, kFade, &quarter_fade8, 4) ||
            fade_one != no_fade || fade_four_at_quarter != quarter_no_fade ||
            quarter_fade8 == quarter_no_fade) {
            std::fprintf(stderr, "%d-bit short-window threshold changed unexpectedly\n", bitdepth);
            return 1;
        }
    }

    constexpr A_long plate_size = 161;
    constexpr A_long plate_left = 40;
    constexpr A_long plate_right = 121;
    constexpr A_long plate_center = 80;
    constexpr unsigned edge_band = kFade + kBlur + 4;
    std::vector<A_u_char> plateau_no_fade, plateau_fade;
    if (!render(32, 0, &plateau_no_fade, 1, plate_size, plate_size, true) ||
        !render(32, kFade, &plateau_fade, 1, plate_size, plate_size, true)) {
        std::fprintf(stderr, "opaque-plateau render failed\n");
        return 1;
    }
    const DiffStats plateau_diff = diff_outputs(plateau_no_fade, plateau_fade, 32);

    const size_t center_pixel = (size_t)plate_center * (size_t)plate_size + plate_center;
    if (pixel_changed(plateau_no_fade, plateau_fade, center_pixel, 32)) {
        std::fprintf(stderr, "constant-alpha plateau center changed\n");
        return 1;
    }
    unsigned changed_edge_pixels = 0;
    for (A_long y = 0; y < plate_size; ++y) {
        for (A_long x = 0; x < plate_size; ++x) {
            const size_t pixel = (size_t)y * (size_t)plate_size + (size_t)x;
            if (!pixel_changed(plateau_no_fade, plateau_fade, pixel, 32)) continue;
            const bool in_edge_bounds =
                x >= plate_left - (A_long)edge_band && x < plate_right + (A_long)edge_band &&
                y >= plate_left - (A_long)edge_band && y < plate_right + (A_long)edge_band;
            const bool edge = x < plate_left + (A_long)edge_band ||
                              x >= plate_right - (A_long)edge_band ||
                              y < plate_left + (A_long)edge_band ||
                              y >= plate_right - (A_long)edge_band;
            if (!in_edge_bounds || !edge) {
                std::fprintf(stderr, "fade changed outside opaque-shape edge band at %ld,%ld\n", x, y);
                return 1;
            }
            ++changed_edge_pixels;
        }
    }
    if (!changed_edge_pixels || changed_edge_pixels != plateau_diff.changed_pixels) {
        std::fprintf(stderr, "opaque-plateau edges did not change\n");
        return 1;
    }
    std::printf("32-bit constant-alpha plateau: %u edge pixels, %u alpha/%u RGB channels; center unchanged (band %u)\n",
                changed_edge_pixels, plateau_diff.changed_alpha, plateau_diff.changed_rgb, edge_band);
    std::puts("fade=1 at 100% and fade=4 at 25% are no-ops (scaled reach=1; no taps); fade=8 at 25% responds.");
    std::puts("PASS: PARAMS_SETUP -> SMART_PRE_RENDER -> SMART_RENDER -> checkout/iterate/output for 8/16/32 bpc.");
    return 0;
}

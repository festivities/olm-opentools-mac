# AGENTS.md — olm-opentools-mac

Port of OLM OpenTools (Windows-only AE plug-ins for anime compositing) to macOS.
Starting with **OLM Directional Blur**. Updated regularly as work progresses.

## Status (2026-09-26)

| Item | State |
|---|---|
| Windows reference `OLMDirectionalBlur.aex` (1.1.1, x64) | Reversed via IDA (`idalib` session `72d5e5dd`) |
| `OLMDirectionalBlur/` macOS source (EffectMain, PiPL, plist, CMake) | Written, **not yet compiled on a Mac** |
| Build/test in AE 2026 Mac | TODO — needs Xcode + AE SDK 26.5 MacOS |
| Bit-exact validation vs Windows render | TODO — render-compare stills |

## What the Windows binary is

- PE64 DLL (56,832 bytes, MD5 `9633e5e109f6ecc1981defb4e07855c6`), entry `entryPointFunc` (not `EffectMain`).
- Classic **SmartRender CPU** effect, 8/16/32-bit paths (`sub_180004A20` / `sub_180003C90` / `sub_1800057B0`), OpenMP row-parallel (`VCOMP140`), no GPU.
- PF spec 13.29, version 1.1.1 (`my_version` 559104), `out_flags` `0x06000040` (USE_OUTPUT_EXTENT | DEEP_COLOR_AWARE | SEND_UPDATE_PARAMS_UI), `out_flags2` `0x08001408` (PARAM_GROUP_START_COLLAPSED | SMART_RENDER | FLOAT_COLOR_AWARE | THREADED).
- PiPL (from `.rsrc`): Name `OLM DirectionalBlur`, Category `OLM Plug-ins`, Match Name `OLM Directional Blur` (note spacing differs), Spec 13/29.
- `PF_Cmd_RENDER` (11) is a no-op; real work is in `SMART_PRE_RENDER` (23) / `SMART_RENDER` (24); `UPDATE_PARAMS_UI` (14) toggles noise params.

## Params (22, decoded from `sub_180007310`; defaults are binary-faithful)

0 Input layer · 1 Angle (ANGLE, 0) · 2 Brightness Gain (FLOAT 0–10, slider 0–2, dflt 1.0) · 3 Size Variation (FIX %, 0–100, 0) · 4/8 Front group · 5 Front Blur Strength (0–4000, 0) · 6 Front Alpha Fade (0–100, 0) · 7 Front Sharp Tail (FIX %, 0) · 9/13 Back group (same trio) · 14 Noise group · 15 Noise Variation (FIX %, 0) · 16 Noise Type POPUP `Smooth | Block | Layer` (dflt Smooth) · 17 Noise Layer (LAYER) · 18 Seed (1–1000, dflt 1) · 19 Offset (ANGLE, 0) · 20 Thickness (FLOAT 1–100, dflt 10.0).

Checkout quirks preserved (`sub_180006C50`): angle truncated to integer degrees then `(deg+90)/180*PI`; offset `deg/36`; FIX % params `/100`; noise mode 1=off / 2=layer / 3=generated, plus `isSmooth` flag.

## Algorithm (as ported)

Rotate input by −angle (alpha-weighted bilinear, `sub_180001EC0`) → size-variation weight per pixel (`sub_180001000`, `pow(alpha, size_var)`) → front/back directional splats with Gaussian LUT `exp(−i²/(2(n/3)²+eps))` (`sub_180001830`), sharp-tail exponent, noise modulation, `fmaxf` edge fade (`sub_1800013E0`/`sub_1800038D0`) → brightness gain → rotate back → restore source alpha (transparent pixels ignored). Noise: 101-entry MSVC-`rand()` table + smoothstep value-noise lattice mixed with white noise, clamped 0–1 (`sub_1800034E0`); Smooth = smoothstep-bilinear, Block = nearest (`sub_180003370`); Layer = noise-layer luma. `UPDATE_PARAMS_UI` hides Seed/Offset/Thickness for Layer mode and hides Noise Layer otherwise (HIDDEN flag, `sub_1800081C0`).

## Files

- `OLMDirectionalBlur/OLMDirectionalBlur.h` — param IDs, `OLMDBParams`.
- `OLMDirectionalBlur/OLMDirectionalBlur.cpp` — full effect (About/GlobalSetup/ParamsSetup/UpdateParamsUI/PreRender/SmartRender, float pipeline, format converters). OpenMP pragmas optional.
- `OLMDirectionalBlur/OLMDirectionalBlurPiPL.r` — Mac Intel+ARM PiPL, identity values above.
- `OLMDirectionalBlur/Mac/OLMDirectionalBlur.plugin-Info.plist` — `eFKT`/`FXTC` bundle.
- `OLMDirectionalBlur/CMakeLists.txt` — build with `-DAE_SDK=<SDK>/Examples`, universal `arm64;x86_64`, Rez PiPL step.

## Conventions for agents

- Keep 1:1 fidelity: same param order/names/defaults, same flags, same match name. Binary wins over the user manual (e.g. `Alpha Fade`, popup default Smooth=1).
- No Windows-isms (`strncpy_s`, VCOMP, `entryPointFunc`); use `PF_STRNNCPY`, optional OpenMP, `EffectMain`.
- Verify against SDK headers under `.opencode/AfterEffectsSDK_26.5_MacOS/.../Examples/Headers` before changing suite calls.
- Never commit secrets; only touch `OLMDirectionalBlur/` and `.agents/` unless asked.
- Reference-only dirs (do not edit): `.opencode/AfterEffectsSDK_*`, `.opencode/olm-opentools-windows/`.

## Next steps

1. Build on a Mac: `cmake -S OLMDirectionalBlur -B build -DAE_SDK=<sdk>/Examples -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" && cmake --build build --config Release`.
2. Smoke-test in AE 2026 (apply, scrub Seed/Thickness, Layer-noise mode, 8/16/32-bit).
3. Pixel-compare Mac vs Windows renders; closest open fidelity risks: LUT index rounding in splats, MSVC-`rand()` sequence assumptions, size-pass group centre/width terms.

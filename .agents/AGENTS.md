# AGENTS.md — olm-opentools-mac

Port of OLM OpenTools (Windows-only AE plug-ins for anime compositing) to macOS.
Starting with **OLM Directional Blur**. Updated regularly.

## Status (2026-09-26, second pass)

| Item | State |
|---|---|
| Windows `OLMDirectionalBlur.aex` (1.1.1, x64) | **Fully re-decompiled** (idalib session `ece2754d`, DB `OLMDirectionalBlur.aex.i64`) |
| `OLMDirectionalBlur/` macOS source | Rewritten from the decompile; **compiles clean** with `g++ -fsyntax-only -Wall -Wextra` against the SDK headers; not yet built in Xcode |
| MT19937 (noise RNG) | Verified against mt19937 reference vectors |
| AE 2026 Mac smoke test + Windows pixel-compare | TODO |

**Warning to future agents:** the first pass of this port (commit `8cf0800`) contained a
plausible-looking but *invented* render core (MSVC `rand`, rotate/`0.5+0.5*noise` weights,
sum-normalized taps). It was replaced in this pass. Trust the decompile, not the prose.

## Build

Out-of-source only (CMakeLists refuses in-source):

```
cmake -S OLMDirectionalBlur -B build \
  -DAE_SDK="/abs/path/AdobeAfterEffectsSDK_26.5_MacOS/Examples" \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

- Include dirs must contain `Headers`, `Headers/SP` (SPBasic.h), `Util`, `Resources`.
- Do **not** define `AE_OS_MAC` on the command line — AEConfig.h defines it; a redefinition
  is a warning and was previously a source of confusion.
- The SDK helper `.cpp` files (`Smart_Utils.cpp`, `AEGP_SuiteHandler.cpp`,
  `MissingSuiteError.cpp`) must be compiled into the target or the link fails.
- AppleClang has no OpenMP: fine, the port has no OpenMP (see below).

### macOS packaging (learned the hard way — AE silently ignores wrong layouts)

Verified against a known-working, Xcode-26-built Mac AE plug-in
(`D:\Dev\projects\F-s-PluginsProjects_forMac\whiteInOut`, inspect its
`Mac/build/.../whiteInOut.plugin`):

- The plug-in executable contains **no PiPL and no `__TEXT,__rsrc`** at all.
- The PiPL lives in **`Contents/Resources/<ProductName>.rsrc`** — a classic resource-fork
  image stored in the file's **data fork** (header: `dataOff=0x100, mapOff=…`).
- Xcode produces it with (exact args from its build manifest; `-arch` matches the build):
  `Rez -o <bundle>/Contents/Resources/<name>.rsrc -d SystemSevenOrLater=1 -useDF -script Roman
   -arch arm64 -arch x86_64 -i <SDK>/Headers -i <SDK>/Headers/SP -i <SDK>/Resources <name>PiPL.r`
- `-useDF` is essential: it keeps the resource image in the data fork so the bundle stays
  code-signable. Default Rez output goes to a resource-fork **xattr**, which (a) does not survive
  `xattr -cr` and (b) makes `codesign` fail with "resource fork, Finder information, or similar
  detritus not allowed".
- The bundle is ad-hoc signed (`codesign --force --sign -`) after the .rsrc is in place.
  Apple Silicon refuses to load binaries whose signature is broken.
- Without a readable PiPL, AE's Effect Manager simply never lists the effect (no error dialog).

If the plug-in still does not appear: check `Rez` ran, then
`ls build/OLMDirectionalBlur.plugin/Contents/Resources/` and
`codesign -dv --verbose=2 build/OLMDirectionalBlur.plugin`.

## Identity / PiPL (decoded from the PE `.rsrc` PiPL)

- PiPL Name `OLM DirectionalBlur` (no space), Category `OLM Plug-ins`,
  **Match Name `OLM Directional Blur`** (with space), Kind `AEEffect`, entry `EffectMain` (Mac).
- Spec version 13.29; PiPL version 2.0; `my_version` = 559104 = `PF_VERSION(1,1,1,DEVELOP,0)`.
- `out_flags` = 0x06000040 = USE_OUTPUT_EXTENT | DEEP_COLOR_AWARE | SEND_UPDATE_PARAMS_UI.
- `out_flags2` = 0x08001408 = PARAM_GROUP_START_COLLAPSED | SUPPORTS_SMART_RENDER |
  FLOAT_COLOR_AWARE | SUPPORTS_THREADED_RENDERING.
- Windows entry is `entryPointFunc`; About prints `OLMDirectionalBlur v1.1.1` with description
  literally `Empty sample for you to mess around with`.

## Command dispatch (entryPointFunc)

ABOUT=0, GLOBAL_SETUP=1 (sets version/flags + `AEGP_RegisterWithAEGP`), PARAMS_SETUP=4,
RENDER=11 (no-op), UPDATE_PARAMS_UI=14 (param hiding), SMART_PRE_RENDER=23 (checkout input
layer, copy `result_rect`/`max_result_rect`; **no** param checkout, **no** pre_render_data),
SMART_RENDER=24 (checkout params there, then dispatch on `extra->input->bitdepth` 8/16/32).
No GET_EXTERNAL_DEPENDENCIES handler (the two "…Dependencies requested." strings in the
binary are used elsewhere/not reachable through the entry point).

## Parameters (ParamsSetup, binary-exact)

0 Input · 1 Angle ANGLE 0 · 2 Brightness Gain FLOAT 0–10 / slider 0–2 / dflt 1.0 / prec 2 ·
3 Size Variation FIX% 0–100 dflt 0 prec 1 PERCENT · 4 Front topic · 5 Blur Strength SLIDER
0–4000 dflt 0 · 6 Alpha Fade SLIDER 0–100 dflt 0 · 7 Sharp Tail FIX% 0–100 dflt 0 ·
8 end · 9 Back topic · 10/11/12 same trio · 13 end · 14 Noise topic · 15 Noise Variation
FIX% 0–100 dflt 0 · 16 Noise Type POPUP **num_choices=2**, dephault 1, items
`"Smooth | Block | Layer"` (bug-compatible: binary ships 2 with a 3-item string) ·
17 Noise Layer LAYER dflt none · 18 Seed SLIDER 1–1000 dflt 1 · 19 Offset ANGLE 0 ·
20 Thickness FLOAT 1–100 dflt 10.0 prec 2 (binary also sets curve_tolerance=10.0) · 21 end.
`num_params` = 22 (written at PARAMS_SETUP).

Checkout quirks (sub_180006C50): Angle & Offset use `(short)(fixed>>16)` integer degrees
(angle → `(deg+90)/180*PI`, offset → `deg/36`); FIX% params `/100`; Noise Type popup value
sets internal mode: `noise_var==0 → 1 (off)`, else `popup==Layer → 2`, else `3 (generated)`;
`smooth = (popup==1)`.

## Render pipeline (all three formats share one float pipeline; kernels
sub_180004A20=8-bit, sub_180003C90=16-bit, sub_1800057B0=32-bit; core sub_1800038D0)

1. Iterate the **output extent** (`PF_iterate8/16/FloatSuite` v1, area =
   `&output->extent_hint`, progress_final = extent height) converting the input world to an
   **RGBA float canvas** (R,G,B,A layout, values 0..1; 8-bit /255, 16-bit /32768, float raw).
   Canvas is diagonal-sized: `diag=sqrtf(W²+H²)`, `my=2-(int)(-0.5*diag)-H/2`,
   `mx=2-(int)(-0.5*diag)-W/2`, canvas `(W+2mx)×(H+2my)`; input pixel (x,y) → canvas
   `(x+mx, y+my)`.
2. Layer noise mode: checkout param 17, require `noise.w==ceil(in_data->width*scale)` and
   `noise.h==ceil(in_data->height*scale)` where `scale = downsample_x.num/den`; build a plane
   of **premultiplied luma** (`green*a*0.587 + red*a*0.299 + blue*a*0.114`, PF pixel order is
   A,R,G,B) for input-world pixels, offset by `input origin - noise origin`, zero outside.
   Generated noise mode: build a lattice field, cell = Thickness (scaled), grid
   `W/cell+3 × H/cell+3`; per cell: white noise + smoothstep-interpolated 101-entry random
   table at index `rand*100+offset (mod 100)`, clamp 0..1; RNG is **std::mt19937**
   (seed=Seed; sample = `genrand_int32() * 2^-32`, zero-extended).
3. Rotate canvas by `+angle` (alpha-weighted bilinear, interior-only guard; inverse map
   `sy=(x-cx)*s+(y-cy)*c+cy`, `sx=(x-cx)*c-(y-cy)*s+cx`); rotate the noise plane if present;
   copy rotated canvas back over the source canvas.
4. If any of (front/back Sharp Tail, Size Variation) ≥ 1e-4: run-length connected components
   of the `alpha>0` mask (links only between adjacent rows, mutual child lists), per-component
   stats written to a 4-float plane: `[area, minRow, (min+max)/2, maxRow-mid]` where
   **maxRow starts at 0** (single-run components get a negative extent — binary-faithful) and
   `maxlevel = max area`. Otherwise fill the stats plane with 1.0 / maxlevel 1.0.
5. LUTs (`exp(-i²/(2(n/3)²+1e-5))`): front/back **blur** LUTs (length = Blur Strength) and
   front/back **fade** LUTs (length = Alpha Fade).
6. Blur core per row (serial; the shipped Windows binary contains no OpenMP fork — only a
   stray `omp_get_max_threads()` call):
   - size pass: `w = pow(area/maxlevel, Size Variation)`; windowed alpha average
     `v71 = (a + Σ LUTfade[k/w]·a(x±k)) / (1 + Σ LUTfade[k/w])`, forward reach
     `(int)(front_fade·w)` clamped to `W-x`, backward to `x`, taps `k=1..reach-1` reading
     `alpha(x+k)` / `alpha(x-k)`; writes premultiplied RGBA + `v71` into the weight buffers.
   - scatter passes: weight `v28 = w · lerp(1, noise, Noise Variation)`;
     `fade = fmaxf(0, 1 - |y-mid|·tail/half)`; front LUT scatters toward **-x**, back LUT
     toward **+x**; per tap `w_tap = alpha·LUT[k/v28]`, RGB accumulates `w_tap·srcRGB`,
     destination alpha takes `max`, weight accumulates into the sum buffer.
7. Normalize RGB by the sum buffer (`>0`), zero the source canvas.
8. Rotate back by `-angle` (B→A).
9. Iterate the output extent writing: `RGB = min(Brightness Gain·RGB, 1)` (integer formats
   `(int)(v·255)` / `(int)(v·32768)` truncating), alpha raw.

Downsampling: blur strengths/fades and Thickness scaled by `downsample_x.num/den`
(`(int)(v*scale)` for the ints); **before** that, the kernel's very first action is
`in_data->utils->copy(effect_ref, input_world, output_world, NULL, NULL)` (the 5-arg
unidentified call at the start of all three kernels). Do not drop it: it is what makes the
"all four blur/fade values are 0" early-out harmless (layer passes through unchanged on
Windows; without the copy the output is left unwritten and the layer disappears in AE 26.5).

## UPDATE_PARAMS_UI (sub_180007EA0 + sub_1800081C0)

At UPDATE_PARAMS_UI: checkout Noise Type; `AEGP_GetEffectLayer` + `AEGP_GetLayerCurrentTime`
(discarded) via PFInterfaceSuite/LayerSuite version 11 (= LayerSuite5); get effect ref;
`AEGP_SetDynamicStreamFlag(stream, AEGP_DynStreamFlag_HIDDEN=2, undoable=false, set)`:
- Smooth/Block → hide **Noise Layer** (param 17)
- Layer → hide **Seed, Offset, Thickness** (18/19/20)

## Known deliberate deviations from the binary

1. Noise field table index: original underflows for negative Offset (OOB table read); the
   port wraps negatives (`t += 100`) instead.
2. The kernel's `utils[8]` sanity call `(effect_ref, input_world, output_world, 0, 0)` was not
   reproduced (purpose unidentified; result only aborts the render on error).
3. Rows are processed serially in one loop (identical output; only the original's chunking is
   cosmetic).
4. Memory: canvases/planes are allocated like the original (≈66 bytes per canvas pixel,
   canvas ≈ (W+H)²), i.e. hundreds of MB for 4K layers. Same as Windows.

## Files

- `OLMDirectionalBlur/OLMDirectionalBlur.h` — param IDs, `OLMDBParams`, identity macros.
- `OLMDirectionalBlur/OLMDirectionalBlur.cpp` — entire effect (MT19937, noise, components,
  rotations, size/scatter passes, format iterate callbacks, AEGP UI, entry).
- `OLMDirectionalBlur/OLMDirectionalBlurPiPL.r` — Mac PiPL (Intel + ARM), identity above.
- `OLMDirectionalBlur/Mac/OLMDirectionalBlur.plugin-Info.plist` — `eFKT`/`FXTC` bundle.
- `OLMDirectionalBlur/CMakeLists.txt` — universal build + Rez step.

## Conventions for agents

- Binary wins over prose and over the user manual. Re-verify every claim against the `.aex`
  (idalib DB path: `.opencode/olm-opentools-windows/OLMDirectionalBlur/OLMDirectionalBlur.aex.i64`).
- Keep `PF_Pixel` channel order in mind: PF pixels are A,R,G,B; the effect's internal canvas is
  R,G,B,A.
- No Windows-isms (`strncpy_s`, VCOMP, `entryPointFunc`); use `PF_STRNNCPY`, `EffectMain`,
  `AEGP_SuiteHandler(in_data->pica_basicP)` (Mac takes `const SPBasicSuite*`, not in/out data).
- Reference-only dirs (do not edit): `.opencode/AfterEffectsSDK_*`, `.opencode/olm-opentools-windows/`.

## Next steps

1. Build on a Mac (commands above) and smoke-test in AE 2026: defaults (pass-through), Angle,
  front/back Blur Strength + Alpha Fade + Sharp Tail, Size Variation, all three Noise Types
  (incl. Noise Layer), 8/16/32-bit projects.
2. Pixel-compare against Windows renders (same project) — differences should be within
  float-rounding noise, except for the documented deviations.
3. If the noise field index safety wrap matters for parity, match the original OOB behaviour
  behind a flag once the exact table-adjacent bytes in the original buffer are known.

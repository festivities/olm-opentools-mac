# AGENTS.md — olm-opentools-mac

Port of OLM OpenTools (Windows-only AE plug-ins for anime compositing) to macOS.
Plug-ins: **OLM Directional Blur** and **OLM RadialBlur** (working in user Mac AE
testing), **OLM Color Key** (source verified; Mac build/host testing pending).
**Next target: OLM ColorKeep** — see "Next plug-in: OLMColorKeep" below; this file is
the complete handoff context (a new session needs nothing else).

## Status (2026-10-05)

| Item | State |
|---|---|
| Windows `OLMDirectionalBlur.aex` (1.1.1, x64) | Render and Alpha Fade paths examined with idalib; original reference is read-only |
| `OLMDirectionalBlur/` macOS source | Builds, loads, and renders in AE 2026 on Mac; user confirms Alpha Fade works after clearing AE's cached plug-in |
| MT19937 (noise RNG) | Verified against mt19937 reference vectors |
| Alpha Fade in AE | **Resolved in user testing**: patched plug-in was cached; clearing AE's cache refreshed it and Alpha Fade works |
| Windows `OLMRadialBlur.aex` (1.3.0, x64) | Decompiled by two subagents; all load-bearing facts re-verified in IDA by the main agent |
| `OLMRadialBlur/` macOS source | Builds and works in user Mac AE testing after `d2355dd` fixed PF_UpdateParamUI definitions |
| `OLMColorKey/` macOS source | Decompiled via general agents, main-agent IDA verification/review; MinGW production-path test passes 1,052 checks; Mac build/AE testing pending |
| `OLMColorKeep` | **Next port target.** Reference: `.opencode/olm-opentools-windows/OLMColorKeep/ColorKeep.aex` (note: filename has NO `OLM` prefix) + user manual PDF in the same dir. Not yet decompiled. |
| Windows-versus-Mac pixel comparison | Deferred by user ("99% of the look" is the bar) |

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

Use a distinct build directory for each plug-in to avoid a CMake source-cache mismatch.
ColorKey, from the repository root on the Mac:

```sh
cmake -S OLMColorKey -B build-colorkey \
  -DAE_SDK="/Users/festivity/dev/olm-opentools-mac/.opencode/AfterEffectsSDK_26.5_MacOS/AdobeAfterEffectsSDK_26.5_MacOS/Examples" \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_BUILD_TYPE=Release
cmake --build build-colorkey
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

## Alpha Fade investigation (2026-09-28; resolved in AE)

- IDA: param IDs 6/11 are checked out at `sub_180006C50` (around `0x180006e7a`
  and `0x180006fb4`) into the front/back fade lengths at scratch `+76/+84`.
  The kernels truncate each length after multiplying by `downsample_x`.
  `sub_180001000` starts taps at `k=1` with `k < reach`: an effective reach of
  0 or 1 **cannot** change the result. Reach is truncated from the scaled Fade
  length multiplied by `pow(component_area/max_area, Size Variation)`, so
  smaller groups with Size Variation can require a larger Fade value. At
  default Size Variation, Fade=1 at full resolution or Fade=4 at quarter
  resolution is a no-op even with Blur Strength > 0. This is original Windows
  behavior, not a Mac-only fix.
- The initial post-fix report that Alpha Fade appeared inert was caused by After Effects
  continuing to use its cached plug-in. User cleared AE's cache, the patched binary refreshed,
  and confirmed Alpha Fade works. Do this before investigating an apparent stale plug-in:
  replace the bundle, clear AE's plug-in/cache state, and relaunch AE.
- `OLMDirectionalBlur/test_alpha_fade.cpp` uses a fake AE host but runs production
  `EffectMain` through PARAMS_SETUP → SMART_PRE_RENDER → SMART_RENDER, using
  both blur strengths = 8, fade 0 vs 8, mixed alpha, and 8/16/32-bpc writers.
  All three outputs change. With a 161×161 constant-alpha plateau, 2,268
  pixels change only in the edge band, alpha only; center RGB/alpha do not
  change. The test also verifies the short-window no-op cases. On Windows,
  from `OLMDirectionalBlur/`, run it with MinGW and the Windows 26.5 SDK:

  ```cmd
  set "SDK=D:\Dev\projects\olm-opentools-mac\.opencode\AfterEffectsSDK_26.5_win\Examples"
  g++ -std=c++17 -D_WIN32 -D_WINDOWS -Wno-multichar -Wno-unknown-pragmas -Wno-missing-field-initializers -ffunction-sections -fdata-sections -I"%SDK%\Headers" -I"%SDK%\Headers\SP" -I"%SDK%\Util" -I"%SDK%\Resources" test_alpha_fade.cpp "%SDK%\Util\AEGP_SuiteHandler.cpp" "%SDK%\Util\MissingSuiteError.cpp" "%SDK%\Util\Smart_Utils.cpp" -Wl,--gc-sections -o "%TEMP%\alpha-fade-test.exe" && "%TEMP%\alpha-fade-test.exe"
  ```
- IDA `sub_180001830` supports the float/double rounding correction in
  `build_lut()`. The user-confirmed cache refresh resolved the reported AE symptom;
  the fake-host test remains a regression check, not a substitute for host testing.
- Windows-versus-Mac pixel-exact parity has not been verified.

## OLMRadialBlur (user reports working after UI-definition fix)

Decompiled from `.opencode/olm-opentools-windows/OLMRadialBlur/OLMRadialBlur.aex`
(1.3.0, 638 funcs). Reference: `.aex.i64` in the same directory. Verified facts:

- **PiPL**: resource id 16000 in the PE `.rsrc` (raw offset 0x2D6B0). Name/Match
  `OLM RadialBlur`, Category `OLM Plug-ins`, spec 13.29, version 622592
  (1.3.0), out_flags `0x06008040` (adds CUSTOM_UI vs DirectionalBlur),
  out_flags2 `0x08001408`, entry `entry_point` (port exports `EffectMain`).
- **Dispatch**: entry 0x18000F970; PARAMS_SETUP `sub_1800034D0` (31 params, ids
  1-31 with 26=Repeat Border and 28-31 = Offset Mode/Offset pairs inserted
  inside the groups), UPDATE_PARAMS_UI `sub_180003CB0`, SMART_PRE_RENDER
  `0x180004020`, SMART_RENDER `0x180004100` (bitdepth switch at 8/16/32 →
  kernels 0x180006970/0x180006160/0x180007180). RENDER is a no-op; no sequence
  data. About: "OLM RadialBlur 1.3\rApply circular and directional blur".
- **UPDATE_PARAMS_UI** does two things: AEGP dynamic-stream HIDDEN for noise
  params (id 21 shown only for Noise Type=Layer; 22/23/24 hidden then — same
  pattern as DirectionalBlur), plus PF_ParamUtilsSuite3 `PF_UpdateParamUI`
  DISABLE: ids 29/28 and 31/30 disabled unless Blur Type=Rotation; ids 4/8
  (Strength) disabled when Rotation+Override mode.
- **Param checkout** `sub_180007AE0` → scratch: +32 blurType, +40/+48 center
  doubles (PointParamSuite value scaled by downsample den/num), +56 brightness,
  +60 noiseVar/100, +64 sizeVar/100 (+68 active >1e-4), +72 scale
  (downsample_x.num/den), +80 noiseType, +84/88 +92/96 offset mode/offset,
  +100/+104 strengths, +108/+112 fades, +116 repeatBorder, +120 ratio,
  **+124 angle in RADIANS** — both ANGLE params (13 Ellipse Angle and 23 Noise
  Offset) go through `(double)fixed * 0.0000002663161090079238` (=(π/180)/65536)
  at checkout; the cores feed the value straight to cosf/sinf. +128 = 1/quality
  (fallback 0.2 if ≤0), +252 seed, +256 noiseOffset (radians), +260 thickness
  (scaled by +72 in the kernel).
- **Kernels**: first call is the 5-arg `utils->copy` (pass-through); early-out
  unless one of (+100, +104, +88, +96, +108, +112) is nonzero — brightness
  alone does NOT render. No iterate suites: kernels read world pixels directly;
  decode 8-bit /255, 16-bit /32768. Writers: RGB `trunc(min(1, brightness*c) *
  255|32768)`, alpha raw `trunc(a * 255|32768)`.
- **Algorithm**: polar-domain blur. Forward map
  `x = cx + cosA*(R cosθ) − sinA*(R sinθ · ratio)`, inverse
  `u = cosA·dx + sinA·dy; v = (cosA·dy − sinA·dx)/ratio; atan2f(v,u)+2π`. Radial
  rows `low = max(0, trunc(minDist/ratio)−2) … maxDist+2`; angle count
  `trunc(360/qualityStep)`. Zoom layout angle×radius, Rotation radius×angle
  (wraps). Size factor is LINEAR: `(area/maxArea)·sizeVar + (1−sizeVar)` — no
  pow, unlike DirectionalBlur. Noise same MT19937 grid (101 table entries
  drawn first, then 2 draws/cell; table offset = the Offset param's radian
  value; `j = 100*draw + offset`, wrap ≥100, OOB for negatives).
- **Direction conventions (verified, easy to get wrong)**: strength scatter —
  inner table toward LOWER indices, outer toward HIGHER (zoom 0x180009D50
  taps [r8-4]/[rdx+4]; rotation sub_180001C90 dir 1 = a4−1). Edge-fade pass —
  the OPPOSITE: outer fade toward LOWER indices, inner toward HIGHER
  (zoom sub_18000A4D0 LUT@+16096 backward; rotation sub_180002780
  LUT@+240112 backward). The initial implementation had zoom fades inverted;
  fixed after IDA verification.
- **Rotation specifics**: strengths/offsets/fades scaled by double
  `0.2/qualityStep` then cvtts2si; strength LUTs fixed length 30000; offset
  combined with row offset `trunc(offset*rows/2/(row+1))` by mode (Add/Max/
  Override), capped 3000; LUT index `k * (30000/reach integer div)`.
- **LUT builder** `sub_18000AA00` (differs from DirectionalBlur's!):
  `den0 = 2·(n²·0.11111112f)` in float, `denom = float(double(den0)+1e-5)`,
  `inv = float(1.0/double(denom))`, `lut[i] = expf(-(float)(i*i) · inv)`.
  SIMD rcp/Newton path skipped (~1 ulp).
- Noise layer luma (type 3): premultiplied `R·A·0.299 + G·A·0.587 + B·A·0.114`
  (channel premul in float, weighted sum in double), aligned by world origins.
- **Deviations**: serial loops (original chunks are serial too), std::vector
  instead of PF handles, negative noise-table wrap, scalar LUT path.
- Test: `OLMRadialBlur/test_radial_blur.cpp` (fake host, real EffectMain;
  31-param setup check, UPDATE_PARAMS_UI callback checks for all six controls
  enabled and disabled, default pass-through, Zoom/Rotation deltas, repeat
  border, 8/16/32 bpc). MinGW command mirrors the DirectionalBlur one (no
  iterate suites needed; links Smart_Utils/AEGP_SuiteHandler/MissingSuiteError).

## OLMColorKey (2.3.1; source verified, Mac host test pending)

Analyzed a TEMP copy, not the reference: original
`.opencode/olm-opentools-windows/OLMColorKey/OLMColorKey.aex`;
IDA working copy `C:\Users\fes\AppData\Local\Temp\opencode\OLMColorKey-analysis.aex`.
Decompilation delegated to two general agents; main agent verified key facts via
`ida-mcp`, then planned/delegated implementation and independently reviewed/tested it.
No .ps1 execution or package installation. Reference directory remains read-only.

### Evidence and identity

- Imports/exports survey: one effect export `entry_point` at `0x180010010`;
  `DllEntryPoint` is the PE loader entry, not a second effect export. CRT/MSVCP/
  VCRUNTIME and KERNEL32 runtime imports; math includes atan2f/fmodf/powf/sin/sinf.
  No VCOMP/OpenMP or GPU rendering imports. AE access is through PICA callbacks.
- PiPL data at RVA `0x2C0B0`, resource ID 16000: Name/Match `OLM Color Key`,
  Category `OLM Plug-ins`, spec 13.29, PiPL 2.0, version 1148928
  (`PF_VERSION(2,3,1,DEVELOP,0)`); raw flags `0x02000440` / `0x08001400`.
  The second flag word includes threaded rendering, SmartRender and float color;
  it does NOT include the group-collapse marker. Mac entry is `EffectMain`.
- Port deliberately adds SEND_UPDATE_PARAMS_UI to the first flag word
  (`0x06000440` in both GlobalSetup and PiPL), per the SDK requirement for UI
  update commands. Preserves binary SUPERVISE flags on Count/Enable Replace.
- No sequence data or legacy RENDER work. SmartPreRender `0x18000B560` unions
  checkout rectangles using `0x1800140C0`; no additional output flags/data.
  SmartRender `0x1800018E0`; 8/16/32 kernels `0x180009440/8F90/98F0` all copy
  input to output first, then key (defaults do NOT mean pass-through).

### Parameters and UI

- Setup `0x180001000`: 224 positions including input, 23 static controls,
  25 slots of 8 controls at `24+8*n` (n=0..24). Position is not uu.id!
  `OLMColorKey.h` records both; duplicate topic/end ID16 matches the binary.
- Static positions: 1 Keep; 2 Threshold; 3 topic; 4 Premultiplied; 5 Space;
  6 Force Precision; 7 Per Color; 8 Per Component; 9..11 global component
  thresholds; 12 end; 13 Thin topic; 14 Thin Amount; 15 Distance; 16 end;
  17 Blur topic; 18 Blur Amount; 19 Distance; 20 Direction; 21 end;
  22 Count; 23 Enable Replace.
- Slots: Use Color, Use Replace, Color, Replace Color, scalar Threshold,
  three component Thresholds. Defaults: Count1, key/replace colors black,
  Use Color true, all thresholds0, Thin/Blur0, RGB, Full precision, Around blur.
  Thin valid -4000..4000 / slider -100..100; Blur valid0..4000 / slider0..100;
  Count valid0..25 / slider0..30 (quirk). Port rejects invalid counts before
  indexing fixed arrays, even if a permissive host would serve extra checkouts.
- UI `0x180001A50`: disables global Threshold when either threshold mode is on;
  global component sliders visible only Per Component && !Per Color.
  Color and Use Color visible for all active slots. Scalar slot thresholds
  visible Per Color && !Per Component; component slot thresholds visible when
  BOTH modes are enabled. Use Replace visible active && Enable Replace;
  Replace Color additionally requires slot Use Replace. Disables Color,
  Use Replace and Replace Color when slot Use Color is off.
  Disabled changes copy the real host ParamDef; visibility uses AEGP HIDDEN.

### Render math and edge pipeline

- Checkout `0x18000A370`: float colors via PF_ColorParamSuite1, not raw-byte
  decoding of color parameters. Epsilon: 8bpc1/510, 16bpc1/65536, float1e-6;
  forced precision lowers epsilon resolution without quantizing input pixels.
- Keyers `0x180001E00/29D0/35D0`: first enabled match wins; keep matched alpha,
  else zero. Keep OFF final callback writes input.alpha-output.alpha.
  RGB stays source unless Keep && Enable Replace && matched slot Use Replace.
  Premultiplied mode changes only the RGB matching copy (RGB*=alpha).
- Spaces: RGB, HSV, Lab76, Lab94, YUV, YCrCb. Main verified RGB/HSV tolerance,
  Lab conversion `0x180009EE0`, Lab distance `0x1800043A0/4510`, and chroma
  distance `0x180004850/4910` independently. Preserve observed quirks:
  HSV component hue wrap is one-sided; aggregate HSV hue is unwrapped;
  YUV/YCrCb ignore the third channel; Lab conversion has no gamma correction
  and uses the binary's unusual coefficients. Lab76 always shifts pixel/key
  a/b in place; Lab94 shifts ONLY in component mode. The pixel triple is not
  reset across enabled slots, so repeated shifts affect later Lab comparisons.
- Lab94 aggregate uses UNSHIFTED Lab and includes **dL squared**, then scaled
  dC squared and hue-angle difference squared. Initial implementation used
  dC squared as the first term; main review caught it and an independent
  gray-lightness production-path regression now detects that mistake.
- With Thin/Blur off: key/invert iterates directly over output extent.
  Otherwise: three cleared PF worlds (two project-depth, one float distance),
  extent-limited keying, inner-boundary mask (8 neighbors; out-of-bounds ignored),
  distance map, thin, canvas copy, optional blur, final keep inversion.
- Box and Approximate chamfers cap at4000 (initial no-site value3999);
  preserve binary's backward-pass min(down,right)+wy quirk. Box considers
  diagonal neighbors with vertical weight; Approximate uses axis steps.
  Euclidean uses separable O(WH) FH squared-distance transform
  `0x18000A710/A920` with denX/denY SQUARED in the parabola cost, then sqrt;
  no4000 cap, no-site distances approximately1e10. Scratch vectors replace
  PF handles; canvas worlds retain SDK allocation/disposal and native precision.
- Thin<0 erodes if -amount>distance; Thin>0 restores full ORIGINAL source
  pixel when amount>=distance (not nearest-edge color). Blur changes only alpha
  via Inside/Around/Outside sine profiles; if keyed alpha0 and profile nonzero,
  use source alpha for the multiplication. Around gives half alpha at distance0.
  Integer output truncates; no extra RGB clamps or premultiplication.
- Safety deviations: reject invalid counts; one-dimensional Box canvases use
  guarded 1D propagation instead of original OOB reads; explicit input checkin
  even on suite/allocation exceptions; RAII world disposal.

### Verification

`OLMColorKey/test_color_key.cpp` runs production EffectMain with fake AE suites,
including UI/stream records, native-depth worlds, iterators and color parameters.
Main agent independently compiled with MinGW C++17 (-Wall -Wextra, SDK-only
warnings suppressed) and ran it: **1,052 checks pass**. Covers 224 identities,
all UI mode combinations/definition preservation, key/keep/replace, six spaces,
threshold/precision/premultiplied modes, distance and edge profiles, downsampling,
padded rows, partial extents, degenerate dimensions and injected-failure cleanup.
Use the DirectionalBlur test compile recipe with `test_color_key.cpp` as source;
link the same three SDK helper cpp files. Mac universal build, Rez/codesign and
actual AE host behavior remain unverified until the user tests the bundle.

## Known deliberate deviations from the DirectionalBlur binary

1. Noise field table index: original underflows for negative Offset (OOB table read); the
   port wraps negatives (`t += 100`) instead.
2. Rows are processed serially in one loop (identical output; only the original's chunking is
   cosmetic).
3. Memory: canvases/planes are allocated like the original (≈66 bytes per canvas pixel,
   canvas ≈ (W+H)²), i.e. hundreds of MB for 4K layers. Same as Windows.

## Bug log (things that shipped broken and why)

- Missing PiPL → AE never listed the effect. The PiPL must live in
  `Contents/Resources/<name>.rsrc` (see packaging section), not in the executable.
- Disappearing layer with default parameters → the kernels' first action is
  `utils->copy(input, output)`; without it the all-zero early-out left the output unwritten.
- **AE `PF_UpdateParamUI` wrong ParamDef type** → `SetParamDisabled()` zero-initialized a
  `PF_ParamDef` and set only `ui_flags`, leaving the type `PF_Param_LAYER` (0)
  instead of the current slider or popup definition. AE's suite accepts only cosmetic fields
  from a valid current definition. It now copies the corresponding `params[]` entry and changes
  only `PF_PUI_DISABLED`; the regression test exercises all six targets enabled and disabled
  through a fake `PF_ParamUtilsSuite3` callback, checking type, name, slider/popup ranges and
  choices, parameter/UI flags, and that the host's original definitions remain unchanged.
- **"Alpha Fade fades the whole image uniformly"** → in `size_pass()` the neighbour alpha reads
  mixed a *pixel* index (`c.pix(x, y)`) with *float* offsets (`k * 4u`), so the fade window
  sampled wrong pixels (≈1/4 of the intended position, often transparent canvas), collapsing
  every pixel's windowed alpha to ≈0.03. The backward expression could also underflow
  `size_t`. Fixed by reading `c.A[c.pix(x ± k, y) * 4u + 3u]`.
  Lesson: the binary indexes the canvas in *float* units (`a3 + 4*v15 + 12` where
  `v15 = 4*pixel`); always convert pixel↔float explicitly in the port.
- After updating a plug-in, AE may continue displaying/using its cached copy. The user confirmed
  the Alpha Fade fix worked after clearing AE's cache; check cache/restart before concluding a
  rebuilt plug-in has no effect.

## Files

- `OLMDirectionalBlur/OLMDirectionalBlur.h` — param IDs, `OLMDBParams`, identity macros.
- `OLMDirectionalBlur/OLMDirectionalBlur.cpp` — entire effect (MT19937, noise, components,
  rotations, size/scatter passes, format iterate callbacks, AEGP UI, entry).
- `OLMDirectionalBlur/OLMDirectionalBlurPiPL.r` — Mac PiPL (Intel + ARM), identity above.
- `OLMDirectionalBlur/Mac/OLMDirectionalBlur.plugin-Info.plist` — `eFKT`/`FXTC` bundle.
- `OLMDirectionalBlur/CMakeLists.txt` — universal build + Rez step.
- `OLMRadialBlur/` — same six-file layout; `test_radial_blur.cpp` is its
  production-path test.
- `OLMColorKey/` — effect header/source, PiPL, Mac plist, CMake and
  `test_color_key.cpp`; see the ColorKey section above for evidence and checks.

## Porting workflow (proven on three plug-ins — follow it for the next one)

1. **Decompile by delegation**: copy the reference `.aex` to
   `%TEMP%\opencode\<Name>-analysis.aex` and open THAT with `ida-mcp`
   (`open_database`); never let IDA touch the read-only reference dir. Launch
   two parallel `general` subagents: one for scaffolding (entry dispatch, PiPL
   resource bytes, full PARAMS_SETUP table with index-vs-uu.id, UI logic), one
   for the render pipeline (kernels, param checkout scratch layout, math).
   Forbid both from renaming/annotating/saving the IDB or editing the workspace.
2. **Main agent verifies** every load-bearing claim in IDA before planning:
   PiPL bytes, dispatch table, checkout function + transitive value-extraction
   helpers (subagents twice missed conversions inside helpers — e.g. fixed-angle
   → radians), constants, and direction/LUT assignments. Subagent reports have
   contained real errors each time; treat them as leads, not truth.
3. **Main agent writes the plan** into one implementation subagent prompt:
   verified facts inline (addresses, offsets, formulas), deliverables
   (`<Name>.h/.cpp`, `<Name>PiPL.r`, `Mac/*.plugin-Info.plist`, `CMakeLists.txt`,
   `test_*.cpp`), and the requirement to consult the IDB for anything unclear.
4. **Main agent verifies the implementation**: read the code against the binary
   (spot-check the risky math), run the MinGW syntax check AND compile+run the
   production-path test independently, fix found bugs (directly or via the same
   subagent session), then update this file.
5. **Commit+push by delegation** (user's standing preference): stage only the
   plug-in dir + `.agents/AGENTS.md`; never `.opencode/`.

Test/build recipes (Windows host, MinGW + the Win 26.5 SDK — cmd expands `%VAR%`
at parse time, so use literal paths in one-liners):

```cmd
g++ -std=c++17 -D_WIN32 -D_WINDOWS -Wno-multichar -Wno-unknown-pragmas -Wno-missing-field-initializers -ffunction-sections -fdata-sections -I "D:\Dev\projects\olm-opentools-mac\.opencode\AfterEffectsSDK_26.5_win\Examples\Headers" -I "D:\Dev\projects\olm-opentools-mac\.opencode\AfterEffectsSDK_26.5_win\Examples\Headers\SP" -I "D:\Dev\projects\olm-opentools-mac\.opencode\AfterEffectsSDK_26.5_win\Examples\Util" -I "D:\Dev\projects\olm-opentools-mac\.opencode\AfterEffectsSDK_26.5_win\Examples\Resources" test_<name>.cpp "<SDK>\Util\AEGP_SuiteHandler.cpp" "<SDK>\Util\MissingSuiteError.cpp" "<SDK>\Util\Smart_Utils.cpp" -Wl,--gc-sections -o "%TEMP%\<name>-test.exe" && "%TEMP%\<name>-test.exe"
```

(Add `-fsyntax-only` + the plug-in `.cpp` alone for the quick syntax check. The
test `#include`s the plug-in `.cpp` and fakes the AE host while running the real
`EffectMain` — see `OLMColorKey/test_color_key.cpp` for the most complete fake
host: iterate suites, world suite, color/point param suites, AEGP suites, and
UI/error-injection scenarios.)

User ground rules (standing): delegate decompilation/implementation/commits to
`general` subagents but verify as main agent; no `.ps1` execution and no pip
installs without explicit permission; `.opencode/` is read-only reference.

## Conventions for agents

- Binary wins over prose and over the user manual. Re-verify every claim against the `.aex`
  via `ida-mcp` on a TEMP copy (references stay untouched; earlier sessions left
  `.aex.i64` DBs next to the originals — reusable but do not modify the tracked files).
- Keep `PF_Pixel` channel order in mind: PF pixels are A,R,G,B; the effect's internal canvas is
  R,G,B,A.
- No Windows-isms (`strncpy_s`, VCOMP, `entryPointFunc`); use `PF_STRNNCPY`, `EffectMain`,
  `AEGP_SuiteHandler(in_data->pica_basicP)` (Mac takes `const SPBasicSuite*`, not in/out data).
- Reference-only dirs (do not edit): `.opencode/AfterEffectsSDK_*`, `.opencode/olm-opentools-windows/`.

## Next plug-in: OLMColorKeep

- Reference: `.opencode/olm-opentools-windows/OLMColorKeep/ColorKeep.aex`
  (NO `OLM` prefix in the filename) + `ColorKeepUserManualEN.pdf`.
- It is the sibling of OLMColorKey (same vendor framework: expect the same
  `MyEffect` C++ scaffolding, `entry_point` export, PiPL in the PE `.rsrc`,
  5-arg `utils->copy` first in kernels, SmartRender-only, AEGP-registered UI
  logic). The ColorKey decompile notes above map the framework; the color-space
  conversions (HSV/Lab/YUV/YCrCb helpers) are very likely shared code — diff
  against ColorKey's verified constants before re-deriving.
- Follow the "Porting workflow" section. Deliverables land in a new
  `OLMColorKeep/` directory mirroring `OLMColorKey/` (six files).
- Watch for the ColorKey-class gotchas: index-vs-uu.id param tables, transitive
  conversions inside checkout helpers, `PF_UpdateParamUI` needing a copy of the
  real host `PF_ParamDef`, pre-render rect unions, and quirks that must be
  reproduced (one-sided hue wrap, ignored channels, in-place Lab shifts).

## Next steps

1. Port OLMColorKeep per the section above (decompile → verify → plan →
   implement → verify → commit+push, all via the delegation workflow).
2. Build ColorKey on the Mac (commands above), install the `.plugin`, clear AE's
   cache/relaunch and smoke-test key/keep, colors/threshold modes, replacement,
   UI visibility and Thin/Blur in 8/16/32-bpc projects. RadialBlur now works in user testing.
3. Windows/Mac pixel comparisons are deferred by user; visually close output is
   the current goal, not a measured claim of bit-exact parity.
4. If the noise field index safety wrap matters for parity, match the original OOB behaviour
   behind a flag once the exact table-adjacent bytes in the original buffer are known.

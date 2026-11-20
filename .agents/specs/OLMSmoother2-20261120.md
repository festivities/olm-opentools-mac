# OLM Smoother v2: 2.1.0 → 2.2.0 (Windows binary 2026-11-20) — verified spec

Sources
- NEW: `%TEMP%\opencode\OLMSmoother2-20261120-analysis.aex` (197,120 B), IDA instance `9492d785201c` (verified `idaapi.get_input_file_path()`).
- OLD: `%TEMP%\opencode\OLMSmoother2-analysis.aex` (192,512 B), IDA instance `81011fbaf7ad`.
- Function identity: masked-byte hashes (500/748 new functions byte-identical to old), normalized disassembly diffs, Hex-Rays pseudocode. The reference `.opencode` files were never opened.
- Nothing was renamed, annotated or saved in either IDB.

## 1. Identity, PiPL, entry, vtable

- PiPL `AE_Effect_Version` = **1114112 = 2.2.0** at RVA 0x180031162 (old 1081344 = 2.1.0).
- PiPL `out_flags` 0x02000440 (0x18003118A) and `out_flags2` 0x08001400 (0x18003119E): unchanged.
- Runtime GLOBAL_SETUP out_flags: binary 0x02000440. Port keeps the documented +SEND_UPDATE_PARAMS_UI (0x06000440). out_flags2 is identical.
- About: sub_18000AD30 formats `"%s %d.%d\r%s"` with args (2, 2) → "OLM Smoother v2 2.2 / Smooth images." (old 2.1).
- Entry sub_18000ADE0 (2084 B; old 0x180009F10): same case set and same vtable slots as old. Cases: 0 ABOUT, 1 GLOBAL_SETUP, 3 GLOBAL_SETDOWN, 4 PARAMS_SETUP, 5-8 SEQUENCE_*, 9 DO_DIALOG, 10 FRAME_SETUP, 11 RENDER, 12 FRAME_SETDOWN, 13 USER_CHANGED, 14 UPDATE_PARAMS_UI, 15 EVENT, 16 GET_EXTERNAL_DEPS, 17 COMPLETELY_GENERAL, 18 QUERY_DYNAMIC_FLAGS, 19-21 AUDIO_*, 22 ARBITRARY, 23 SMART_PRE_RENDER, 24 SMART_RENDER, 28 GET_FLATTENED_SEQ, 31 SMART_RENDER_GPU (vtable+32, gpu flag 1; port does not implement, no GPU out_flags2 bits), 32/33 GPU_DEVICE_SETUP/SETDOWN.
- Vtable `??_7MyEffect@@6B@` at 0x180023E38 (old 0x180022D48):

| slot | new | old | note |
|---|---|---|---|
| +0 | 0x180001940 (432 B) | same name | GlobalSetup, LUTs via AEGP memory handles (below) |
| +16 | 0x180006500 (202 B) | 0x180005630 | PRE_RENDER, byte-identical |
| +24 | 0x180001C30 (742 B) | 0x180001C30 (726 B) | PARAMS_SETUP |
| +32 | 0x180001F20 (1160 B) | 0x180001F10 | SMART_RENDER |
| +40 | nullsub_1 | nullsub_1 | USER_CHANGED_PARAM no-op |
| +48 | 0x1800023B0 (407 B) | 0x1800023A0 (275 B) | UPDATE_PARAMS_UI (changed) |
| +56 | 0x1800065F0 (740 B) | 0x180005720 | EVENT (same size, bytes differ only in relocations) |
| +64 | 0x1800068E0 (408 B) | 0x180005A10 | ARBITRARY |
| +72..+96 | 3-byte stubs | 3-byte stubs | sequence commands |

- GlobalSetup 0x180001940 builds the two sRGB LUTs through "AEGP Memory Suite" handles (`olm_aa_cache_sRGB_to_linear` / `..._linear_to_sRGB`, 40000 B). The builders are byte-identical to old: decode 0x18000B7B0 ≡ old 0x18000A8E0, encode 0x18000B610 ≡ old 0x18000A740. Table contents are unchanged.
- Imports added: `cbrtf` (0x1800231E0), `omp_get_max_threads`, `_vcomp_set_num_threads`. OpenMP is used only for worker scheduling, not for numerics (port stays serial).

## 2. Parameters (PARAMS_SETUP 0x180001C30) — position = add order, id = uu.id

| pos | id | name | type | range / default / precision / flags |
|---|---|---|---|---|
| 1 | 1 | Enable Color Key | checkbox | dflt 0 |
| 2 | 2 | Color Key | color | white |
| 3 | 15 | Invert Color Key | checkbox | dflt 0 |
| 4 | 3 | Smoothness | int slider | 0..100, dflt 100 |
| 5 | 4 | Extra Smooth | int slider | 0..100, dflt 0 |
| 6 | 5 | **Smooth Range** | **FLOAT_SLIDER (type 10)** | valid 0..100, slider 0..100, **dflt 2.0 (0x40000000)**, prec 1, flags 0 (old: int slider dflt 2) |
| 7 | 6 | **Smoother Version** | popup | **3 items "v1 (sRGB)\|v2 (Linear)\|v2 (Oklab)"**, dflt 2, **flags 64 SUPERVISE** (old: 2 items "v1\|v2", flags 0) |
| 8 | 7 | Gamma Correction | popup | None\|Gamma Colors\|All Colors, dflt 1, SUPERVISE |
| 9 | 8 | Gamma Value | float slider | 1.0..2.4, dflt 2.4, prec 2 |
| 10 | 9 | Number of Gamma Colors | int slider | 0..5, dflt 1, SUPERVISE |
| 11-15 | 10-14 | Gamma Color ×5 | color | black |

- num_params = 16 (`*(a3+48)=16`).
- Smooth Range flags 0 (no USE_VALUE_FOR_OLD_PROJECTS). Port matches. Old int project values are handled by the host; not testable here.
- **"Enable Local Contrast Adaptation" is a dead string, not a parameter.** Literal at 0x180023578 and 0x180023CF0 (new), 0x180022560 and 0x180022C28 (old). Zero code or data xrefs in both binaries (checked pinned). It is not in PARAMS_SETUP (num_params 16). The manual does not mention it. **No Local Contrast Adaptation feature exists in 2.2; nothing was implemented.**

## 3. UPDATE_PARAMS_UI (sub_1800023B0, 407 B) — call order (verified by UI record parity)

Version = sub_180001520(id 6); Oklab iff popup value == 3. Calls to PF_UpdateParamUI, in order:
1. id 2 Color Key ← Enable off
2. id 15 Invert ← Enable off
3. id 7 Gamma Correction ← Oklab
4. id 8 Gamma Value ← Oklab
5. id 8 Gamma Value ← (Oklab || mode==None) (second call, binary-exact)
6. id 9 Count ← (Oklab || mode != Gamma Colors)

Then stream hides (sub_180006E70) for ids 10..14 with visible = !Oklab && mode==Gamma Colors && i<count.
Old (0x1800023A0): ids 2, 15, 8 (mode None), 9 (mode != Colors), then pickers. The gamma-correction id 7 disable is new.

## 4. Parameter checkout (sub_180005C60, 998 B; old sub_180004E10 862 B)

- The ctx passed to kernels is `params+8` (sub_180006050 = `lea rax,[rcx+8]`). Params struct is 272 B (old 248). `+264` = pointer to xmmword_18002E980 (LUT struct {count 10000, table}).
- Version: `params+12` = popup − 1 (1→0 sRGB, 2→1 linear, 3→2 Oklab). Other values: binary aborts; port returns BAD_CALLBACK_PARAM.
- `params+40` (float) = `sub_180001500(id5) * 0.01f`. This is the smooth threshold base.
- `params+44` = Smoothness int (id 3); `params+48` = Extra int (id 4).
- Oklab: returns before reading Gamma Correction/Value/Count and colors (gamma off, `params+80` stays 0).
- `params+136` = 0 always (edge post-pass disabled on the live path). `params+140` = 2.0f (unused on live path).

## 5. Render pipeline (2.2 differences marked **)

Kernels 0x1800045F0 / 0x180004D50 / 0x1800049A0 (8/16/32 bpc, 944 B each; old 949 B each): same error messages, same empty-ROI path.
- Stage A, canvas conversion: 0x1800036B0/3780/3850 → 0x180002550/2B30/30D0. Uses the converter table below.
- Stage B, worker fork: 0x18000C110 → 0x18000BF70.
- Bbox fast path 0x18000D000: shrinks the processed rect when the active area is < 50 %. Not ported. e2e shows 0 diffs on sparse images.
- Stage E, per-pixel driver and writer: 0x180004350/4430/4510 → 0x180003920/3CA0/3FC0.

**Converter table** (per-pixel driver, v46[3·row+col]). Row = canvas space (`a8[1]`), col = output space (`a8[0]`).

| entry | function | meaning |
|---|---|---|
| [0], [4], [8] | 0x180005480 | identity |
| [1] | 0x180005B90 → 0x1800059D0 | sRGB → linear (LUT path) |
| [2] | 0x180005BC0 | sRGB → Oklab (decode, then 0x180005520) |
| [3] | 0x1800057A0 → 0x1800056D0 | linear → sRGB (LUT encode) |
| [5] | 0x1800056B0 → 0x180005520 | linear → Oklab |
| [6] | 0x180005990 | Oklab → sRGB (0x180005840, then encode) |
| [7] | 0x180005970 | Oklab → linear |

**Oklab forward, 0x180005520 (396 B)** — float throughout, cube roots by UCRT `cbrtf`:
- `l = cbrtf((G*0.53633255f + R*0.41222146f) + B*0.051445995f)`
- `m = cbrtf((R*0.2119035f + G*0.6806995f) + B*0.10739696f)`
- `s = cbrtf((R*0.08830246f + G*0.28171885f) + B*0.6299787f)`
- `L = (l*0.21045426f + m*0.7936178f) - s*0.004072047f`
- `a = (l*1.9779985f - m*2.4285922f) + s*0.4505937f`
- `b = (m*0.78277177f + l*0.025904037f) - s*0.80867577f`
- alpha copied.

**Oklab inverse, 0x180005840 (298 B)**:
- `lp = ((a*0.39633778f) + L) + b*0.21580376f`; `mp = (L - a*0.105561346f) - b*0.06385417f`; `sp = (L - a*0.08948418f) - b*1.2914855f`
- `l = (lp*lp)*lp`, `m = (mp*mp)*mp`, `s = (sp*sp)*sp`
- `R = ((l*4.0767417f) - (m*3.3077116f)) + s*0.23096994f`
- `G = ((m*2.6097574f) - (l*1.268438f)) - s*0.34131938f`
- `B = (s*1.7076147f) - ((l*0.0041960864f) + m*0.7034186f)`
- alpha copied.

**sRGB LUTs** (live path for linear and Oklab): decode 0x1800053E0 ≡ old 0x180004CD0; encode 0x180005340 ≡ old 0x180004C30. The formula branches 0x1800059D0 / 0x1800057D0 are not taken on the live path (ctx flag nonzero).

**Canvas per mode**: v1 = sRGB (no LUT). v2 linear = LUT decode. **v2 Oklab = LUT decode → linear → Oklab** (key match stays before decode, as in 2.1). Oklab values then flow through edges, MLAA and blend unchanged.

**Edge builder 0x18000C1D0 (743 B; old 0x18000AED0, 770 B)**
- b0 = (x ≥ 1) && d(p, left) ≥ thr
- b1 = (y ≥ 1) && d(p, up) ≥ thr
- b2 = (y ≥ 1, x ≥ 1) && d(p, up-left) ≥ thr
- **b3 = (y ≥ 1) && (x < W−1) && d(p, up-right) ≥ thr. 2.1 used x+1 < W−1. This is the one real MLAA-side change.**
- thr = `ctx[+0x20]` + 0.001f (`addss xmm4, dword_180023ECC`), where ctx[+0x20] = `params+40` = float(Smooth Range) × 0.01f.

**Distance 0x18000C5A0 (395 B; old 0x18000B370, 325 B)**: same value as old: `fmaxf(max(|dR|,|dG|,|dB|), |ΔL|) + |dA|`, with both-transparent → 0. New extra arg a3 (canvas-space index): if ==2 the luma term is R-only. Equivalent, because |ΔL| ≤ max|Δc| for weights summing to 1.

**MLAA dispatcher 0x18000D380 (2537 B; old 0x18000C340, 2537 B)**: byte-identical to old after normalizing operand size prefixes and jump-table labels. Only difference: four MOVD displacements shifted by +4 (struct relayout). Reads smoothness int at ctx+36 and extra at ctx+40 and divides by 100.0 (double).
- All MLAA families, corners, run-gated corners, stair handlers (0x180011660/11A60/11E40/12100/123A0/12680/12910 etc.), sample append (0x1800115F0 ≡ old 0x180010590) and end-type (0x180011670 ≡ old 0x180010610) are byte-identical after normalization. Samples cap at 12 in port; a 64-cap variant gave identical output.

**Per-pixel glue 0x18000DDE0 (448 B; old 0x18000CDA0, 412 B)**: D380 → [if `(a8+80) != 0`: 0x18000CE40 gamma decide] → **0x18000D1B0 (encode-premultiply** ≡ old C190 plus rule: gamma pow only when canvas space ≤ 1 **and** apply flag; premultiply center and samples by alpha when alpha ≠ 1) → **0x18000BE70 (mix** ≡ old ABC0) → **0x18000C4C0 (finish: unpremultiply when alpha ∉ {0,1}; gamma pow only when canvas ≤ 1; no clamp** (2.1 B1E0 clamped here)) → writer.

**Writer, in 0x180003920 (per pixel)** — **clamps every output channel including alpha: x ≥ 0 ? fminf(x,1) : 0** (NaN → 0). Premultiply-on-output only if writer byte +29 is set (always 0, dead). Quantize `(int)(x*scale + 0.5)` (8 bpc scale 255; 16/32 per port, verified by e2e). Oklab output = OklabToLinear → SrgbEncode (LUT) → clamp.

## 6. Port changes that implement the above

Files changed (all under OLMSmoother2AE/, nothing committed):
- `OLMSmoother2.h`: 2.2.0 version/about; `OLMS2_Version` enum; params struct `version` (0/1/2), `smooth_threshold` (float).
- `OLMSmoother2.cpp`: PARAMS (float Smooth Range, 3-item popup, SUPERVISE), UPDATE_PARAMS_UI (new order and rules), CheckoutParams (version, threshold, Oklab skips gamma), `LinearToOklab`/`OklabToLinear`, `ApplyColorSpace` (replaces ApplyGammaDecode), `BuildEdges` (b3 rule, threshold), `BlendFinish` (`clamp_rgb` flag: no RGB clamp for Oklab), `WriterClamp` + `WritePixel` (Oklab output path, all-channel clamp), `#pragma clang fp contract(off)`, `OLMS2_CBRT` hook (default `std::cbrt`).
- `OLMSmoother2PiPL.r`: `AE_Effect_Version` 1114112.
- `Mac/OLMSmoother2.plugin-Info.plist`: CFBundleShortVersionString 2.2.0.
- `test_smoother2.cpp`: new expectations (float Smooth Range, 3-item popup, version 1114112, about 2.2, UI sequence incl. duplicate id-8 record and Oklab rows, b3 rule), plus `verify_oklab()` (white/black/round-trip).

## 7. Verification

- **MinGW fake-host unit test** (`test_smoother2.cpp`, Win 26.5 SDK): **999 checks, 0 failures**. No warnings from the port with `-Wall -Wextra`.
- **E2E** (`%TEMP%\opencode\olm-e2e\smoother-20261120\e2e_smoother.cpp`, binary = TEMP copy of the 2.2 .aex, log `run7.txt`):
  - 8,358 counted render runs (8/16/32 bpc; 42 parameter sets incl. Oklab, fractional Smooth Range, the full version × gamma-mode matrix; 8 geometries) → **0 differing runs, 0 differing bytes**, err-diff 0.
  - HDR 32 bpc out-of-range input: 294 runs, 0 diffs.
  - Randomized 64×48 stress: 1,008 runs, 0 diffs.
  - PARAMS_SETUP defs: 15/15 identical (type, flags, ui, name, id, dflt, range, popup strings).
  - UPDATE_PARAMS_UI: 5 sets, 0 record/stream differences.
  - sRGB LUT parity: 0 differing entries (decode and encode).
  - my_version 0x00110000 both; about text identical; out_flags2 identical.
  - No unserved AcquireSuite requests.
- **Bin-vs-bin diagnosis** (`%TEMP%\opencode\olm-e2e\bvb\bvb.exe`, 2.2 vs 2.1 binaries): the 3×3 exhaustive alpha search (512 patterns) localized the b3 change to x = W−2 (pixel (1,1): alpha 32 binary vs 26 port). After the fix the port matches the binary.
- **Oklab residual, explained and closed under the test hook:**
  - Inverse (0x180005840) vs port `OklabToLinear`: 0 / 200,000 bit-exact.
  - Forward (0x180005520) vs port with `std::cbrt`: 137,519 / 200,000 pixels differ, by ≤ 1 ULP (max 5.96e-7). Apple/MinGW `cbrtf` is not the UCRT function the binary imports.
  - Forward with the Windows `ucrtbase!cbrtf`: **0 / 200,000**. The harness sets `OLMS2_CBRT` to that function; the Oklab matrix then has 0 diffs at 8/16/32 bpc.

## 8. Residuals and risks

1. **Mac build cube root.** The Mac build uses `std::cbrt` (Apple libm), so Oklab forward differs from Windows by ≤ 1 ULP (≤ 1 LSB at 8/16 bpc on about two thirds of pixels). Bit-exactness on Mac would need a port of UCRT `cbrtf`. Not done.
2. Documented port deviations kept: serial loops, sample cap 12 (no observed effect), +SEND_UPDATE_PARAMS_UI, bbox fast path not ported (0 diffs observed), SMART_RENDER_GPU not implemented, binary `abort()` on bad popup becomes BAD_CALLBACK_PARAM.
3. Old-project loading of Smooth Range (int → float) is not testable here. The binary sets no USE_VALUE_FOR_OLD_PROJECTS flag and the port matches.
4. The old harness (`%TEMP%\opencode\olm-e2e\smoother\e2e_smoother.cpp`) is untouched but now compiles against the 2.2 port. It no longer represents 2.1 behaviour.

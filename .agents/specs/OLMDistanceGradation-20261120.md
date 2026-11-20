# OLM Distance Gradation: 0.8.2 ALPHA -> 0.8.3 (DistanceGradation-20261120.aex)

Binaries
- OLD: `.opencode/olm-opentools-windows/OLMDistanceGradation/DistanceGradation-20260304.aex`
  (sha256 8d1b5f86...). IDB `%TEMP%\opencode\OLMDistanceGradation-analysis.aex.i64`,
  IDA instance 8edf1f36071b (input path checked).
- NEW: `.opencode/olm-opentools-windows/OLMDistanceGradation/DistanceGradation-20261120.aex`
  (sha256 cb627ab4...). TEMP copy `%TEMP%\opencode\OLMDistanceGradation-20261120-analysis.aex`,
  IDA instance 9037e112babd (input path checked).
- Addresses are NEW unless marked OLD.

Method: Hex-Rays pseudocode of every plug-in function; disassembly of the gradation
normalise/threshold block; PE section/resource diff (`tools/pe_dump.py`, `rsrc_cmp.py`);
whole-DB function fingerprints (sha1 of instruction bytes, rel32/RIP and absolute
displacement/imm fields zeroed): only the 14 plug-in functions, entryPointFunc and
cvCopyMakeBorder (sub_181275EC0) differ in code. Behaviour was checked with the
differential harness (section 16).

## 1. Summary of changes (all verified)
1. Version 0.8.2 ALPHA -> 0.8.3 DEVELOP; About "v0.82" -> "v0.83".
2. Global out_flags add `PF_OutFlag_I_EXPAND_BUFFER` (0x200). out_flags2 unchanged.
3. NEW param 13 "Use Padding" (checkbox, default off). num_params 13 -> 14.
4. Threshold sliders (ids 3, 4): UI `slider_max` 512 -> 1000 (valid 0..1000, default 128 unchanged).
5. PreRender (new function) computes the padded result rect, sets RETURNS_EXTRA_PIXELS, and
   clears `preserve_rgb_of_zero_alpha` on the request copy. No union with the incoming rect.
6. Compute builds the mask on a padded canvas (cvCopyMakeBorder, value 0), measures the
   distance on the padded full-res size, and writes the padded result top-left into the
   expanded output world.
7. Gradation normalisation changed (section 9): beta = kmax * min(1, max/T) style rule.
8. Both mode: per-pixel masked copy (cvCopy with mask) instead of cvAdd.
9. Composite: input read at (x - pad_x, y - pad_y); out-of-layer input -> alpha 0 and colour 0.
   Refcon fields after +192 shift (section 8).
10. Degenerate test uses the padded area.
11. Gradation issues an extra read-only cvMinMaxLoc-like call (sub_181183720). No output effect.

## 2. PiPL (rsrc type 0x800000A0 id 0x3E80 = PiPL 16000, 310 bytes)
Byte diff (OLD vs NEW): exactly 2 values differ.
- `eVER` AE_Effect_Version (PiPL +0xBA..0xBD LE): 0x00041200 -> 0x00041800.
  PF_VERSION decode (bits maj<<19 min<<15 bug<<11 stage<<9 build): OLD = 0.8.2 stage 1
  (ALPHA) = 266752; NEW = 0.8.3 stage 0 (DEVELOP) = 268288.
- `eGLO` AE_Effect_Global_OutFlags (PiPL +0xE2..0xE5 LE): 0x06000040 -> 0x06000240
  (adds bit 0x200 = PF_OutFlag_I_EXPAND_BUFFER, AE_Effect.h:981).
- Unchanged: kind AEEffect; name "Distance Gradation"; category "OLM Plug-ins"; entry
  entryPointFunc (Windows) / EffectMain (port); PiPL 2.0; spec 13.29; eGL2 0x08001400;
  match "OLM Distance Gradation"; manifest (type 0x18) identical.
- Export `entryPointFunc` ordinal 1: OLD RVA 0x1174BC0, NEW RVA 0x11755A0.

## 3. Entry dispatch (PF_Cmd values from AE_Effect.h:1243)
ABOUT 0, GLOBAL_SETUP 1, UNUSED_0 2, GLOBAL_SETDOWN 3, PARAMS_SETUP 4, RENDER 11,
USER_CHANGED_PARAM 13, UPDATE_PARAMS_UI 14, SMART_PRE_RENDER 23, SMART_RENDER 24.

| cmd | handler | OLD | NEW | status |
|---|---|---|---|---|
| 0 | ABOUT | "%s v%d.%d%d\r%s" (0, 8, 2) | same format (0, 8, 3) | text changed |
| 1 | GLOBAL_SETUP | version 266752, out_flags 0x06000040, out_flags2 0x08001400 | version 268288, out_flags 0x06000240, out_flags2 0x08001400 | changed |
| 4 | PARAMS_SETUP | sub_181173720 (1508 B) | sub_181173CC0 (1628 B) | extra param, slider max |
| 11 | RENDER (legacy) | sub_181170280 / 181170380 | sub_181170280 / 181170370 | not ported (section 11) |
| 14 | UPDATE_PARAMS_UI | sub_181174190 (521 B) | sub_181174B20 (521 B) | identical body; extra arg ignored |
| 23 | SMART_PRE_RENDER | inline (pass-through + union) | sub_181174320 (864 B) | changed |
| 24 | SMART_RENDER | sub_181173D10 (827 B) | sub_181174680 (860 B) | changed |

## 4. Params (sub_181173CC0 NEW; sub_181173720 OLD)
Name table `unk_18189E004`, stride 260 (`sub_1811758A0(i) = base + 260*i`). Index 23 = "Use Padding".

| uu.id | type | name | default | range / notes |
|---|---|---|---|---|
| 1 | CHECKBOX | Invert | 0 | unchanged |
| 2 | POPUP | In/Out | 0 | "Inside\|Outside\|Both" (3) |
| 3 | SLIDER | Inside Threshold | 128 | valid 0..1000, **slider 0..1000 (OLD 0..512)** |
| 4 | SLIDER | Outside Threshold | 128 | valid 0..1000, **slider 0..1000 (OLD 0..512)** |
| 5 | POPUP | Render Mode | 1 | "RGB\|Layer" |
| 6 | CHECKBOX | Use Background Color | 0 | |
| 7 | COLOR | Gradation Color | 255,0,0 | |
| 8 | COLOR | "BG Color " (trailing space) | 0,0,0 | |
| 9 | POPUP | Interpolation Mode | 2 (Linear) | 4 choices |
| 10 | FLOAT_SLIDER | Power | 1.0 | 0.01..5, prec 2 |
| 11 | POPUP | Blur Mode | 1 | 3 names; `num_choices` = 5 in BOTH binaries (port uses 3, see 15) |
| 12 | SLIDER | Blur Size | 0 | 0..4096 / slider 0..500 |
| **13** | **CHECKBOX** | **Use Padding** | **0** | **NEW** |

num_params = 14 (OLD 13). Verified by a byte-level compare of the PF_ParamDef values
(harness PARAMS_SETUP check): all fields match the port except Blur Mode num_choices.

## 5. UPDATE_PARAMS_UI
Body sub_181174D30 (939 B) and helper sub_1811749E0 (311 B) are identical to OLD 1811743A0 and
181174050. Entry sub_181174B20 first acquires "AEGP PF Interface Suite" v1 / "AEGP Layer Suite" v11
/ "AEGP Effect Suite" v3 and only then runs the UI body. Disable map (144 combos) matches the port.

## 6. SMART_PRE_RENDER sub_181174320 (NEW). extra = [0] input, [1] output, [2] cb.
1. Checkout param 2 (In/Out) -> v7 (1 Inside, 2 Outside, 3 Both).
2. Checkout param 13 (Use Padding) -> v13 = (value != 0).
3. If Use Padding: r = (int)((float)den_y / (float)num_y); if v7 in {2,3}: checkout param 4 and
   if thr > r then r = thr. Else r = 0.
4. px = (int)((float)(num_x * r) / (float)den_x); py = (int)((float)(num_y * r) / (float)den_y).
   (num/den from in_data->downsample_x/y, num_x @+284, den_x @+288, num_y @+292, den_y @+296.)
5. req = input->output_request (44 bytes); req.preserve_rgb_of_zero_alpha = 0 (byte +24).
   res = cb->checkout_layer(effect_ref, 0, 0, &req, current_time, time_step, time_scale, &res).
6. If res has any nonzero field: result_rect = max_result_rect = (left-px, top-py, right+px, bottom+py)
   and flags |= 1 (RETURNS_EXTRA_PIXELS). Else result_rect = max_result_rect = res (no flag).
Verified by harness (PRE result rect, max rect, flags, request bytes: 0 mismatches in 2716 runs).

## 7. SMART_RENDER sub_181174680 (NEW)
Depth = input->bitdepth (8/16/32). checkout_layer_pixels(0) -> input; checkout_output -> output;
compute(depth, ..., input, output, refcon); render (8: sub_181170370, 16: sub_181170280,
32: iterateFloat + sub_181170CC0). Iterate area = output->extent_hint; src = NULL.
Suites: "PF Iterate8 Suite" v1, "PF iterate16 Suite" v1, "PF iterateFloat Suite" v1.

## 8. Compute (NEW 8: sub_181171F20 3788 B; 16: sub_181171050; 32: sub_181172DF0. OLD 3340 B each)
Refcon (a7) NEW (OLD offset in parentheses where it moved):
- +0 input world, +8 output world, +16 in_data.
- +144 degenerate byte; +148 int In/Out; +152 Gradation colour float RGBA (A +152, R +156, G +160, B +164);
  +168 BG colour float RGBA (A +168, R +172, G +176, B +180).
- +184 int Inside Thr; +188 int Outside Thr; +192 int pad_x; +196 int pad_y.
- +200 byte Use BG (OLD +192); +201 byte Invert (OLD +193); +208 int Render Mode (OLD +200);
  +212 byte Use Padding (new); +216 int Interp (OLD +204); +220 float Power (OLD +208);
  +224 int Blur Mode; +228 int Blur Size.

Padding:
- r as in section 6 step 3 (computed in the compute); px, py as section 6 step 4.
- PW = W + 2*px, PH = H + 2*py (W, H = input world size).
- FW = (int)((float)(unsigned)(den_x*W) / (float)num_x + (float)(2*r)), FH likewise with y.
  (OLD FW = (int)((float)(den_x*W)/num_x), no +2r.)

Pipeline:
1. Input rows (W x H) -> Mat A.
2. cvCopyMakeBorder(A -> B (PW x PH), top=py, left=px, right=PW-px-W, bottom=PH-py-H,
   BORDER_CONSTANT, value 0) (sub_181275EC0 -> sub_181179130; verified pseudocode).
3. mask = (alpha8(B) > 1) ? 255 : 0 (sub_181175370 == OLD 181174990, identical).
   count = cvCountNonZero (sub_181183650 == OLD 181182FF0).
   degenerate = (count == PW*PH) || (count == 0).
4. Gradation (section 9) -> dist (PW x PH from full-res FW x FH).
5. blurred = dist (sub_18117C530 copy, mask 0). If Blur Mode != 1: cvSmooth(dist -> blurred,
   type = mode-1, kx = 2*((num_x*blur)/den_x)+1, ky likewise) on PW x PH.
6. cvMerge(blurred, blurred, blurred, dist) -> cvConvert into a PW x PH 4-channel Mat.
7. Copy-out: top-left out->width x out->height of that result -> output world.

## 9. Gradation (NEW sub_1811750E0 645 B; OLD sub_181174750 570 B)
Callees: NN resize mask->full (sub_1812AF8B0, wrapper == OLD), distanceTransform L2 precise
(sub_1812B1EE0 == OLD 1812B1590), linear resize back, threshold (sub_1812B7380 == OLD 1812B6A30),
normalize (sub_18117D2E0 == OLD 18117CA40). Extra: sub_181183720 (369 B) = cvMinMaxLoc (max only).

Threshold: cvThreshold(src, dst, T, T, type) with
  T (float xmm1) = Constant ? max(1.0, thr) : (thr == 0 ? 0.1 : thr);
  type = BINARY (0) for Constant, TRUNC (2) otherwise.

Normalisation (NEW, disassembly 0x1811752A2..0x181175308):
  maxv = cvMinMaxLoc(thresholded).max (double)
  factor = (T > maxv) ? maxv / T : 1.0
  beta = factor * kmax            (kmax = 255 for 8 bpc, 32768 for 16 bpc, 1.0 for 32 bpc)
  normalize(src, dst, alpha = 0.0, beta, NORM_MINMAX, mask = 0)
OLD: beta = kmax (no factor). Net effect: normalised value = min(d, T) * kmax / T.
Evidence: 4x4 opaque square on 12x10 (Inside, Linear, thr 255): binary alpha 254 at edges (d=1) and
253 in the centre (d=2) = exact distances with beta = 2; OLD rule gave 126 / 0.
The square probes (edge-touching and interior, Inside and Outside, T = 255) match the port exactly.

Inside with thr == 0: the NEW binary does cvConvert(mask -> 32F) + normalize(kmax) (no threshold);
the port does the same. Not isolated by a dedicated probe; the thr=0 failures in the e2e also use
Gaussian blur or downsampling (section 15).

## 10. Both mode (NEW 0x181171F20 case 3)
inside = gradation(mask, InsideThr); invert mask in place;
outside = gradation(~mask, OutsideThr);
dist = cvCopy(inside -> dist, mask = original mask); dist = cvCopy(outside -> dist, mask = ~mask).
sub_18117C530 = cvCopy(src, dst, mask) (copy.cpp error text confirms). OLD: dist = cvAdd(inside,
outside, mask 0) (sub_181182B10). The port implements the masked select
(`dist = orig ? inside : outside` per pixel). Note: add == select only when the inside gradation is
zero wherever mask == 0; that can fail after downsampling, so the masked select is the correct model.

## 11. Composite (per pixel; NEW sub_181170880 8 bit, sub_181170460 16 bit, sub_181170CC0 float)
- Input pixel at (x - refcon[192], y - refcon[196]) from refcon[0]; outside the input layer ->
  alpha = colour = 0 (OLD read (x, y)).
- Gradation read at (x, y) in refcon[1] (output world). Unchanged.
- Non-invert t = clamp(1 - r, 0, 1); invert t = r. Outside alpha = min(1, 1 - inA), negative -> raw.
- Offsets shifted as in section 8. Degenerate branch unchanged.

## 12. Distance transform
Binary precise L2 path (sub_1812B1EE0 -> sub_1812B0010): uses IPP when `sub_18118A670()` (IPP on)
and (`sub_181233E00() <= 1` or size < 16384). `sub_181233E00` returns 1 when no thread count is set,
so IPP runs for all sizes in an unconfigured host. Probes show the binary's distances equal the exact
Euclidean distances (port's trueDistTrans) for interior and edge-touching squares.
Residual: at T <= 1 (Constant, thr 0 or 1) the binary's distance for a 1-px gap compares slightly
above 1 (BINARY T=1 classifies it as "far"); the port's exact 1.0 classifies it as "near". Float rounding
of IPP's distance transform, not reproduced.

## 13. Blur and float arithmetic (residual)
Box (cvSmooth CV_BLUR) and Gaussian (CV_GAUSSIAN) use OpenCV's separable float filter; the port's
double-accumulate / float-order versions differ at 1-LSB (8 bit) and ulp level (16/32 bit).

## 14. Degenerate & outputs
- degenerate = (count == PW*PH) || (count == 0); padding makes the full-count case impossible.
- Copy-out is top-left (no centring).

## 15. Residuals (differential e2e, section 16)
- Blur=Gaussian (CV_GAUSSIAN) and Blur=Box: 1-LSB and ulp differences (OpenCV float filter order).
- 16/32-bit composite and gradation: ulp-level float ordering (32 bit: most differences <= 1e-5).
- Constant interpolation with thresholds <= 1: IPP distance-transform rounding (section 12).
- Blur Mode popup `num_choices` = 5 in both binaries; port keeps 3 (confirmed Mac behaviour).
- Legacy PF_Cmd_RENDER (11) not ported (same as OLD).

## 16. Verification
- Unit (fake host, MinGW, Win 26.5 SDK): `OLMDistanceGradation/test_distance_gradation.cpp`:
  132 checks, 0 failures.
- Differential e2e vs the real 0.8.3 `.aex` (`%TEMP%\opencode\olm-e2e\distgrad\e2e_distgrad.cpp`):
  params (13 defs + count), About, GLOBAL_SETUP, UI (144 combos), PRE (rects, flags, request),
  RENDER (2716 runs, 8/16/32 bpc, In/Out, interp, blur, render, bg, invert, padding,
  downsampling, odd sizes and row/offset pads). Result: 2251 runs bit-identical; 465 runs
  differ (sections 13 to 15). Probes: 8 Linear square probes (Inside and Outside, 4 squares each) are bit-identical;
  the 4 Constant Outside probes with outside thr 0 differ (section 12).

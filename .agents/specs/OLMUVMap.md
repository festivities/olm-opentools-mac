# OLM UV Map 1.1.1 — verified spec (IDA, pinned instance 06f277acc365)

Binary: `%TEMP%\opencode\OLMUVMap-analysis.aex` (PE32+ x64, 40960 bytes, 129 functions).
Every decompile below was re-run pinned to instance `06f277acc365` with
`idaapi.get_input_file_path()` = `...\OLMUVMap-analysis.aex` (19 decompile lengths matched the
first run). Addresses are IDA VAs (image base 0x180000000). Layout facts are from SDK 26.5 Win
headers `AE_Effect.h` / `AE_EffectPixelFormat.h`.

## 1. Identity / PiPL (raw resource bytes)

PE `.rsrc` type `PIPL`, id 16000, lang 0x409, RVA 0xC0B0, file offset 0x9AB0, 290 bytes.
Properties decoded (LE int32, four-char codes byte-reversed as stored):

| key | value |
|---|---|
| `eFKT` kind (vendor MIB8) | AEEffect |
| `Name` | `OLM UV Map` |
| `catg` | `OLM Plug-ins` |
| `8664` | `entryPointFunc` (Windows entry; Mac port uses `EffectMain`) |
| `ePVR` | 2 (PiPL version 2.0) |
| `eSVR` | 13.29 (spec) |
| `eVER` | 559104 = PF_VERSION(1,1,1,DEVELOP,0) |
| `eINF` | 0 (2 bytes) |
| `eGLO` | 0x02000040 (out_flags) |
| `eGL2` | 0x18001400 (out_flags2) |
| `ANMe` | `OLM UV Map` (match name, with spaces) |
| `aeFL` | 0 (reserved) |

Out-flag bits: 0x02000040 = USE_OUTPUT_EXTENT(1<<6) | DEEP_COLOR_AWARE(1<<25).
0x18001400 = SUPPORTS_SMART_RENDER(1<<10) | FLOAT_COLOR_AWARE(1<<12) |
SUPPORTS_THREADED_RENDERING(1<<27) | MUTABLE_RENDER_SEQUENCE_DATA_SLOWER(1<<28).
The binary never sets SEND_UPDATE_PARAMS_UI (1<<26) — see Deviations.

## 2. Dispatch — `entryPointFunc` 0x180005210 (ordinal 1; also exported as DllEntryPoint 0x18002176C)

PF_Cmd numbers (AE_Effect.h): ABOUT 0, GLOBAL_SETUP 1, GLOBAL_SETDOWN 3, PARAMS_SETUP 4,
SEQUENCE_SETUP 5, SEQUENCE_RESETUP 6, SEQUENCE_SETDOWN 8, RENDER 11, USER_CHANGED_PARAM 13,
UPDATE_PARAMS_UI 14, SMART_PRE_RENDER 23, SMART_RENDER 24.

- 0 ABOUT: `"%s v%d.%d.%d\r%s"` with strings[1] `OLM UV Map`, 1.1.1, strings[2]
  `Move pixels around in an image` → `OLM UV Map v1.1.1\rMove pixels around in an image`.
  String table: base 0x18000A004, stride 260 (`sub_180005780`): [1] `OLM UV Map`,
  [2] `Move pixels around in an image`, [3] `Texture`.
- 1 GLOBAL_SETUP: `out->my_version=559104` (+0); `out->out_flags=0x02000040` (+96);
  `out->out_flags2=0x18001400` (+400). Then AEGP register (Utility suite slot 7, name
  `OLM UV Map`, plug-in id stored at 0x18000A500). The id is used only by the binary's
  AEGP stream read in the UI (see §4).
- 4 PARAMS_SETUP `sub_180005210` case 4: three `inter.add_param(effect_ref, -1, def)` calls,
  then `out->num_params = 4` (+48).
- 5 SEQUENCE_SETUP `sub_180003B60`, 6 SEQUENCE_RESETUP inline: dispose old
  `out->sequence_data` (+56) if non-null; `utils->host_new_handle(48)` (utils+160) into +56;
  struct zero-init, `[+40] = -1`; NULL → return 512.
- 8 SEQUENCE_SETDOWN: if `in->sequence_data` (+320) → `utils->dispose_handle` (utils+184);
  `out->sequence_data = 0`.
- 11 RENDER: default case → return 0 (no-op).
- 13/14 USER_CHANGED_PARAM / UPDATE_PARAMS_UI → `sub_180004080` (§4).
- 23 SMART_PRE_RENDER (inline): `extra->input` (+0, 44-byte request copy), `extra->output`
  (+8), `extra->cb` (+16). `cb->checkout_layer(effect_ref, 0, 0, &req, current_time, time_step,
  time_scale, &res)` — index 0 = **input** layer only. Then `UnionLRect(res.result_rect →
  out.result_rect)` and `UnionLRect(res.max_result_rect → out.max_result_rect)`. The texture
  (param 1) is NOT checked out here.
- 24 SMART_RENDER `sub_180003BE0` (§5).

## 3. PARAMS_SETUP (binary-exact, PF_ParamDef offsets: uu.id 0, ui_flags 4, type 12, name 16, flags 48, union 56)

| pos (= uu.id) | name | type | flags | ui | value detail |
|---|---|---|---|---|---|
| 1 | `Texture` | LAYER (0) | 0 | 0 | default -1 = `PF_LayerDefault_MYSELF` (binary sets `v22[43] = -1`; confirmed by e2e) |
| 2 | `Interpolation Method` | POPUP (7) | 0 | 0 | value 3, num_choices 3, dflt 3, choices `Nearest Neighbor\|Bilinear\|Trilinear` |
| 3 | `Smoothness` | FLOAT_SLIDER (10) | SUPERVISE (0x40) | 0 | value 0.5, valid 0..1, slider 0..1, dflt 0.5, precision 2, display 0, fs_flags 0, curve_tolerance 3.0 |

Popup choices string at the literal `0x18000A...` (`Nearest Neighbor|Bilinear|Trilinear`, 3 items).
The manual says "Nearest"; the binary says **Nearest Neighbor**. Binary wins.
Position equals uu.id (1, 2, 3). Input is position 0.

## 4. UPDATE_PARAMS_UI / USER_CHANGED_PARAM — `sub_180004080`

1. Read the popup (Interpolation Method) value at layer time through AEGP stream index 2
   (`AEGP_GetNewEffectStreamByIndex(plugin, effect, 2)` + `AEGP_GetNewStreamValue`, one double
   at +8). Equivalent port: `PF_CheckoutParam(index 2)` at `in->current_time`.
2. `PF_CheckoutParam(index 3)` → Smoothness def (in v32), then
   `ui_flags = (popup != 3) ? 0x20 : 0` (write into +4; 0x20 = PF_PUI_DISABLED, 1<<5). This is an
   overwrite, not OR.
3. `PF_ParamUtilsSuite3.UpdateParamUI(effect_ref, 3, &def)` (suite version 3, slot 0),
   then `in->inter.checkin_param(effect_ref, &def)` (in+8).

Effect: Smoothness greyed out unless Trilinear. The binary's flags lack SEND_UPDATE_PARAMS_UI,
so AE never sends this command in the binary; see Deviations.

## 5. SMART_RENDER — `sub_180003BE0(in, out, extra)`

1. `bitdepth = extra->input->bitdepth` (word at +44 of the input world; 8/16/32).
   Anything else → `return` with no output written.
2. `extra->cb->checkout_layer_pixels(effect_ref, 0, &input)` (cb+0). Then
   `extra->cb->checkout_output(effect_ref, &output)` (cb+16). Errors → return.
   The binary never checks in the input (port deviation).
3. Context `ctx` (v31, passed as refcon) set by `sub_180003750(bitdepth, in, out, extra, output, ctx)`:
   - `ctx+24 = in`, `ctx+16 = output`, `ctx+32 = bitdepth`.
   - `checkout_param(2)` → popup value → `ctx+36` (mode 1 Nearest, 2 Bilinear, 3 Trilinear).
     Checkin.
   - `checkout_param(1)` → texture def; copy its `u.ld` (120 bytes) to `ctx+40 .. ctx+160`.
     Checkin later.
   - If mode == 3: `if (out->sequence_data == NULL)` → `out->return_msg =`
     `"Trilinear invoked without mipmaps"`, `out->out_flags |= 0x100`, return 512.
     Else `sub_180004560` (build mip chain sizes/worlds, §6), then `sub_180004AB0` (fill levels).
   - `checkout_param(3)` → Smoothness value → `(float)` at `ctx+160`.
4. Switch on bitdepth → iterate the output extent (`output->extent_hint`) with the pixel fn
   `sub_180002550` (8), `sub_180001BF0` (16), `sub_180002EA0` (32). Refcon = ctx.
   Input world = `ctx+8`, output = `ctx+16`.
5. If mode == 3 and `out->sequence_data`: dispose mip worlds (`sub_1800049D0`, PF World suite slot 1
   `DisposeWorld` for each of `maxLevel+1` levels).

Layout facts: `ctx+64` ld.data, `ctx+72` ld.rowbytes, `ctx+76` ld.width, `ctx+80` ld.height
(texture `PF_LayerDef` copy at +40; PF_EffectWorld fields as used by the kernels).

## 6. Mip chain

### 6a. Mip object (`sub_180004560`, 2603 bytes)
- Seq struct (48 bytes, handle deref): `[0]=W, [4]=H, [8]=bitdepth, [16]=begin, [24]=end,
  [32]=cap, [40]=maxLevel`. Stored in the sequence handle.
- `maxLevel = floor(log2(max(W,H)))` computed as
  `v14 = (float)((double)logf((float)max) / log(2.0))` then `floorf(v14)`. `count = maxLevel+1`.
- Vector of `PF_EffectWorld` (120 bytes each) resized to `count`.
- Each level `i` in 0..maxLevel: `PF World Suite v2 NewWorld(effect_ref, max(W>>i,1),
  max(H>>i,1), clear=1, PF_PixelFormat_ARGB128 = 'ae32' = 842229089, &world)`. Float RGBA,
  16 bytes/pixel.

### 6b. Mip fill (`sub_180004AB0`, 8282 bytes)
- **Level 0** = texture (W×H). Pixel `[A,R,G,B]` floats:
  - 8 bpc: `(float)byte / 255.0` (A = `(float)A/255.0`, double division). No clamp.
  - 16 bpc: `(float)u16 * 0.000030517578f`.
  - 32 bpc: raw floats, copied.
  Texture pixel address `data + rowbytes*y + bpp*x`, bpp = 4/8/16.
- **Level L ≥ 1** from level L-1 (prev W'=max(W>>(L-1),1), H'; cur w=max(W>>L,1), h):
  - Row taps `sy = {2r, min(2r+1,H'-1), min(2r+2,H'-1)}`; col taps `sx` likewise.
  - Row weights: H' even → `{0.5, 0.5, 0}`; H' odd → `{(h-r), h, (r+1)} / (2h+1)` (float).
  - Col weights: W' even → `{0.5, 0.5, 0}`; W' odd → `{(w-c), w, (c+1)} / (2w+1)` (float).
  - Accumulate per channel, in this order: for each row k (0,1,2): `acc += wy_k*wx0*p[sx0]`,
    then `+= wy_k*wx1*p[sx1]`, then `+= wy_k*wx2*p[sx2]`, each product float, add left to right.
    Initial acc = 0. Output `[A,R,G,B]` (A channel is index 0).

## 7. Per-pixel kernels (mode from ctx+36; UV = clamp(R)·(W-1) in x, clamp(1-G)·(H-1) in y)

Common:
- `R_norm`, `G_norm`: 8 → `/255.0`, 16 → `*0.000030517578f`, 32 raw; then `fmaxf(v,0)`,
  `>=1 → 1`. UV from input pixel R (index 1) and G (index 2).
- Input alpha guard: 8/16 `A != 0` (raw); 32 `A > 0.0f`. Fails → output all zero
  (8/16: `(A,R,G,B)=0`; 32: float zero).
- Texture data NULL → same zero output.
- `factor(A)` = alpha normalised (8 `/255.0`, 16 `*0.000030517578f`, 32 raw), `fmaxf(v,0)`, `>=1`→1,
  `v>0`→v, else 0. Used by Nearest and Bilinear: `out.A = (int)(factor(inA) * texA)`.
  (32 bpc: `out.A = factor*texA` float, no int.)

### 7a. Nearest (mode 1)
- `x = (int)clamp((W-1)*R, 0, W-1)`, `y = (int)clamp((H-1)*(1-G), 0, H-1)` (float math).
- RGB = texture raw bytes/floats copied; A as above. (`sub_180002550` else-branch.)

### 7b. Bilinear (mode 2)
- `x = clamp((W-1)*R)`, `y = clamp((H-1)*(1-G))`; `x0=floor(x)`, `y0=floor(y)`,
  `fx=x-x0`, `fy=y-y0`. Taps `p00=(x0,y0)`, `p10=(x1,y0)`, `p01=(x0,y1)`, `p11=(x1,y1)` with
  `x1=clamp(x0+1)`, `y1=clamp(y0+1)`.
- Blend (float, order exact): `top=(p00*(1-fx)) + (p10*fx)`, `bot=(p01*(1-fx))+(p11*fx)`,
  `res=(top*(1-fy)) + (bot*fy)` with `(float)(1.0-fx)` etc.
- 8 bpc: `sub_1800014A0` (ints): RGB = `(uint8)(int)res`, A = `(uint8)(int)res` (texture bytes).
  Then out.A = `(uint8)(int)(factor(inA) * (float)A_blend)`.
- 16 bpc: `sub_1800011C0` (u16), same structure, uint16 writes.
- 32 bpc: `sub_180001770` (float level sampler, 16-byte stride) → RGBA floats; out.A = factor*A.

### 7c. Trilinear (mode 3) — 8 `sub_180002550`, 16 `sub_180001BF0`, 32 `sub_180002EA0`
- Neighbour samples: `P0`=(x,y), `P1`=(x-1,y), `P2`=(x+1,y), `P3`=(x,y-1), `P4`=(x,y+1), each
  coordinate clamped by `sub_180001A40` (8) / `sub_1800019E0` (16) / `sub_180001AA0` (32).
  Note: the 4 calls at 0x1800026D4..0x180002722 use r8=y, y, y, y-1, y+1 (verified by disasm).
- LOD helper `sub_180004780` (1564 bytes) on `[R,G,_,A]` per pixel (P0..P4 at idx 0,4,8,12,16,
  alpha at 3,7,11,15,19; A raw, not normalised). Neighbour P_k valid if `A_k > 0`.
  For each valid k: `dU_k = |R_k - R0|`, `dV_k = |G_k - G0|` (k = left, right, up, down).
  `maxU = max dU_k`, `maxV = max dV_k` (starting from 0, strict `>`).
  `minU = maxU`, `minV = maxV`; for each valid k with `dU_k > 0 && minU > dU_k` → `minU = dU_k`;
  same for V. Outputs: `maxU`, `minU`, `maxV`, `minV`.
- Footprint in texels: `a = (W-1)*minU`, `b = (W-1)*maxU`, `c = (H-1)*minV`, `d = (H-1)*maxV`.
  `v57 = min(c,d)`, `v59 = b`, `v60 = a` (kernel): `lo = min(v57, v59, v60)`,
  `hi = max(v57, v59, v60)`.
- `footprint = (float)(1-s)*lo + s*hi` (s = Smoothness float at ctx+160). `if (footprint <= 0) footprint = 0.0001f`.
- `lod = logf(footprint) / logf(2.0f)` (float division, both `logf`).
- `lvl0 = clamp(floorf(lod), 0, maxLevel)`, `lvl1 = clamp(floorf(lod)+1, 0, maxLevel)`,
  `t = frac(clamp(lod, 0, maxLevel))`.
- Sample level `k` at `x = clamp(R·(Wk-1))`, `y = clamp((1-G)·(Hk-1))`, Wk = max(W>>lvl,1),
  Hk = max(H>>lvl,1). `sub_180001770(level, (int)x, (int)y, fx=x-floor(x), fy=y-floor(y), out)`.
- `sub_180001770` (float level bilinear, 16-byte stride): clamp `x`, `y`, `x+1`, `y+1` to level,
  blend as in 7b (float), outputs `[A,R,G,B]`.
- Result: RGB = lerp(level0,level1,t); A = lerp(A0,A1,t) * clamp(inA_normalised).
  - 8: `(uint8)(int)(v*255.0)` per channel (v in 0..1), A same with `*255.0`. Truncation.
  - 16: `(uint16)(int)(v*32768.0)`.
  - 32: float, no scale, no clamp.

Output channel mapping: PF pixels are A,R,G,B.

## 8. Bit-depth summary

| bpc | UV norm | factor | Nearest RGB | Bilinear | Trilinear write |
|---|---|---|---|---|---|
| 8 | /255 | /255 | raw byte | `(uint8)(int)` | `(uint8)(int)(v*255)` |
| 16 | *0.000030517578f | same | raw u16 | `(uint16)(int)` | `(uint16)(int)(v*32768)` |
| 32 | raw | raw clamp | raw float | `sub_180001770` float | raw float |

## 8b. E2E result (2026-11-20, `%TEMP%\opencode\olm-e2e\uvmap\e2e_uvmap.cpp`)

Real `OLMUVMap.aex` (LoadLibrary, `entryPointFunc`) vs port `EffectMain`, one fake host.
2532 scenarios (5064 renders): depth 8/16/32 x UV {1x1..33x17} x texture {1x1..64x64} x mode
{1,2,3} x Smoothness {0,0.25,0.7,1} x 2 seeds, plus 8-bpc UV with 16-bpc texture (mixed depth).
Compared: full output buffers (incl. out-of-extent sentinel), PARAMS_SETUP defs (3), About text,
UPDATE_PARAMS_UI records (popup 1,2,3 → Smoothness ui_flags 0x20,0x20,0).
- Found and fixed by the e2e: Texture default must be -1 (`PF_LayerDefault_MYSELF`), not 0.
- Result: 8/16 bpc, all modes, mixed depth: 0 differing bytes. 32 bpc Trilinear: 12 of 2532
  scenarios differ by 31 bytes in total, each a 1-ulp float difference.
- Cause (verified): MinGW's `logf` differs bitwise from the UCRT `logf` the binary imports
  (785 / 2,000,000 random inputs, `logf_probe.cpp`). A mutant port routed through
  `ucrtbase.dll!logf` (`mut/OLMUVMap_ucrtlog.cpp`) gives 0 differing bytes over all 2532 scenarios.
  The residual is host libm only, so it will be different again on Apple libm.
- Flags: binary 0x02000040 vs port 0x06000040 (SEND_UPDATE_PARAMS_UI, the documented deviation).

## 9. Deviations / residuals (for the port)

1. **SEND_UPDATE_PARAMS_UI** not set in the binary; the port adds it (`0x06000040`), matching the
   convention in ColorKey/Smoother/ColorKeep. Without it AE never calls §4 and Smoothness stays
   enabled. Flag for the main agent: revert to `0x02000040` to match the binary exactly.
2. Popup read via `PF_CheckoutParam(2)` at `in->current_time`, not AEGP stream index 2. Same value.
3. No AEGP registration in the port (binary registers only for the stream read).
4. Input is checked in (binary leaks the input checkout).
5. Sequence handle holds no mip cache: mips are rebuilt per render (binary disposes them per render
   too, so output is identical).
6. Serial loops over `output->extent_hint` instead of PF_Iterate suites.
7. `logf`/`floorf` from host libm (same as other ports; possible 1-ulp differences at LOD
   boundaries).
8. Mixed bit depths: the binary reads the texture with the **UV** layer's bit depth and does not
   convert. The port does the same (no conversion); out-of-buffer reads are not guarded in the
   binary; the port clamps only on the layer's own dimensions.
9. Trilinear with a null `out->sequence_data` returns the same error as the binary.

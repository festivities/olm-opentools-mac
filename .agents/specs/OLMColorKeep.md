# OLM Color Keep 1.1.0 (ColorKeep.aex) — verified spec

Binary: `.opencode/olm-opentools-windows/OLMColorKeep/ColorKeep.aex`
(127,488 bytes, MD5 `e8e8eafd620953c137bfbbfe1ec73874`, SHA256 `f378f431…a305`).
IDA working copy: `%TEMP%\opencode\OLMColorKeep-20261120-analysis.aex`
(IDB instance `61c789fd438f`; all IDA calls pinned to that id and MD5-asserted).
Reference, not edited. Image base 0x180000000, PE32+, MSVC, single export `entry_point`
at RVA 0x6690 (VA 0x180006690).

IDA caveat (for future readers): the ida-mcp "current" database is shared between agents
and switched mid-session to another plug-in's IDB (OLMSmoother2). Every fact below was
re-read on instance `61c789fd438f` with the MD5 asserted in the same call. Facts from the
PE file itself (PiPL, strings, exports) were parsed with Python, independent of IDA.

## 1. Identity and PiPL (file-parsed, PE `.rsrc` type `PIPL` id 16000, 294 bytes)

Decoded with `%TEMP%\opencode\olm-e2e\colorkeep\pipl_dump.py` (8BIM records, LE keys).

| Key | New 1.1.0 | Old 1.0.1 | Note |
|---|---|---|---|
| `kind` | `eFKT` | `eFKT` | same |
| `name` (Pascal) | **`OLM Color Keep`** | `Color Keep` | display name changed |
| `ANMe` (match name) | **`OLM Color Keep`** | `OLM Color Keep` | **unchanged: old projects keep matching** |
| `catg` | `OLM Plug-ins` | `OLM Plug-ins` | same |
| `4668` (entry point) | `entry_point` | `entryPointFunc` | Win export name; Mac port uses `EffectMain` |
| `RVPe` (raw key as printed by the decoder) | 2 | 2 | PiPL version |
| `RVSe` | 13.29 (0x0d, 0x1d) | 13.29 | spec version |
| `REVe` | **0x00088000 = PF_VERSION(1,1,0,0,0)** | 0x00080800 = (1,0,1) | effect version (= my_version) |
| `OLGe` out_flags | **0x02000040** | 0x02000040 | unchanged |
| `2LGe` out_flags2 | **0x08001400** | 0x08001400 | unchanged |
| `FNIe` | 2 bytes 00 00 | same | |
| `LFea` | 0 | 0 | |

Flag decode (AE_Effect.h, Win 26.5 SDK): out_flags 0x02000040 = `DEEP_COLOR_AWARE`
(1<<25) | `USE_OUTPUT_EXTENT` (1<<6). **No** `CUSTOM_UI` (1<<15), **no**
`SEND_UPDATE_PARAMS_UI` (1<<26), **no** `PARAM_GROUP_START_COLLAPSED`.
out_flags2 0x08001400 = `SUPPORTS_THREADED_RENDERING` (1<<27) | `FLOAT_COLOR_AWARE`
(1<<12) | `SUPPORTS_SMART_RENDER` (1<<10).

About text: `"%s %d.%d\r%s"` = `OLM Color Keep 1.1` / `Constraint colors`
(sub_1800065E0 @ 0x1800065E0). Old: `Color Keep v1.01`.

## 2. Command dispatch — `entry_point` @ 0x180006690

Switch on the PF_Cmd (SDK enum verified: ABOUT 0, GLOBAL_SETUP 1, GLOBAL_SETDOWN 3,
PARAMS_SETUP 4, SEQUENCE_SETUP 5 … RENDER 11, USER_CHANGED_PARAM 13, UPDATE_PARAMS_UI 14,
EVENT 15, ARBITRARY 22, SMART_PRE_RENDER 23, SMART_RENDER 24, SMART_RENDER_GPU 31).
Most cases forward to a **MyEffect** C++ object (vtable `??_7MyEffect@@6B@` @ 0x1800157F0).

| Cmd | Handler (IDA) | Behaviour |
|---|---|---|
| 0 ABOUT | sub_1800065E0 | writes About text (above) |
| 1 GLOBAL_SETUP | alloc 0x9F0-byte handle → ctor sub_180001840 → init sub_180001960 | stores handle at `out->global_data`(+40) |
| 3 GLOBAL_SETDOWN | sub_180001A50 then dispose sub_180001920 | destroys per-param objects; AEGP Compute Cache acquire/release only, **no cache keys used** |
| 4 PARAMS_SETUP | vt+0x18 = sub_180001000 | §3 |
| 5/6/7/8 SEQUENCE_* | vt+0x48/58/60/50 = `xor eax,eax; ret` (sub_180002140/160/180/150) | no-op |
| 9,10,12,16–21,28,32,33 | direct stubs sub_1800021xx/sub_18000219x/sub_180002200-240 (`xor eax,eax; ret`) | no-op, PF_Err_NONE |
| 11 RENDER (legacy) | sub_1800021E0 = `return 0` | **no-op** (SmartRender only) |
| 13 USER_CHANGED_PARAM | vt+0x28 = nullsub_1 (0x180001C90, `ret`) | no-op |
| 14 UPDATE_PARAMS_UI | vt+0x30 = sub_180001460 | §4 |
| 15 EVENT | vt+0x38 = sub_180001CB0 | acts **only if `param_type == 11` (PF_Param_ARBITRARY_DATA)**; this effect has none → no-op (§6) |
| 22 ARBITRARY_CALLBACK | vt+0x40 = sub_180001FA0 | ae_param arbitrary sub-dispatch; no arbitrary params → no-op |
| 23 SMART_PRE_RENDER | vt+0x10 = sub_180001BC0 | §7 |
| 24 SMART_RENDER | vt+0x20 = sub_1800010D0 (gpu=0) | §8 |
| 31 SMART_RENDER_GPU | vt+0x20 = sub_1800010D0 (extra arg 1, ignored) | same CPU path; GPU not advertised |

Errors: `ae_errors::ErrException`/`AErrException` return their code; `std::exception`
and `...` set `out->return_msg` and `PF_OutFlag…|=0x100` and return. The port needs no
exception machinery: no-op commands return PF_Err_NONE.

## 3. PARAMS_SETUP — sub_180001000

```
AddSlider("Enabled Color Num", uu.id=1, ui_flags=0, flags=SUPERVISE(64), type=PF_Param_SLIDER(1),
          value=1, dephault=1, valid=[0,200], slider=[0,200])        (helper sub_180002FF0)
for i in 0..199: AddColor("Color", uu.id=i+2, ui_flags=0, flags=0, type=PF_Param_COLOR(5),
                          value=dephault=opaque black (A=0xFF,R=0,G=0,B=0))   (helper sub_180002840)
out->num_params = 202                                         (`*(a3+48)=202`)
```
Layout proof: helper writes PF_ParamDef at a 0x90-byte local: uu.id @0, ui_flags @4,
ui_width/height @8/10, param_type @12, name @16 (32 bytes), flags @48, union u @56.
PF_SliderDef: value@u+0, valid_min@u+68 (= a7.lo=0), valid_max@u+72 (=a7.hi=200),
slider_min@u+76 (=a6.lo=0), slider_max@u+80 (=a6.hi=200), dephault@u+84 (=a5=1).
Colour: PF_ColorDef value@u+0 = bytes {A=0xFF, R=a5, G=a6, B=a7} = opaque black; dephault copy @u+4.

Param positions = uu.id (MyEffect `id` array: ctor sets `[0]=0` (input), then 1, 2..201;
count `+816` = 202). Checkout index == uu.id. No SUPERVISE on colours.

## 4. UPDATE_PARAMS_UI — sub_180001460 (only runs if the host sends it; see §9)

1. `count` = checkout(id 1) → `u.sd.value` (sub_180004340), **at current time**.
2. for i in 0..count−1: read colour id i+2 (sub_180003B80 → §5 colour read). Result unused.
3. for id 2..201 (v6 = 0..199): `hidden = !(v6 < count)` via sub_180002530:
   `AEGP_GetNewEffectForEffect(plugin_id, effect)` (sub_180007860) →
   `AEGP_GetNewEffectStreamByIndex(plugin_id, effect, index=id, &stream)` (sub_180007A30, index = uu.id) →
   `AEGP_DynamicStreamSuite5::SetDynamicStreamFlag(stream, AEGP_DynStreamFlag_HIDDEN(1<<1),
   undoable=false, value = hidden)`.
   Plug-in id stored at object+2440 (`a1[610]`), set by RegisterWithAEGP in GlobalSetup.
   `count` is not clamped: count>200 overruns v9[804] (UB in binary); count<0 → all hidden.

## 5. Colour read — sub_180003B80 → sub_180009F40 → PF_ColorParamSuite1

Param checked out at `in->current_time` (+224), `time_step` (+228), `time_scale` (+240)
(sub_18000A2E0). Converted by `PF_GetFloatingPointColorFromColorDef(effect_ref, &def, &fp)`
(sub_180009F40 slot 0). `fp` = `PF_PixelFloat` **{A, R, G, B}** floats, stored as 16 bytes.

## 6. EVENT — sub_180001CB0 and the Drawbot/Ramp code (dead for this effect)

- Reached only via EVENT. Gate: `params[event.index]->param_type == 11`. PF_Param_ARBITRARY_DATA
  (PF_ParamType enum: SLIDER 1, COLOR 5, POPUP 7, CUSTOM 8, NO_DATA 9, FLOAT_SLIDER 10,
  ARBITRARY_DATA 11). PARAMS_SETUP adds only types 1 and 5, so the gate is never true.
- Drawbot draw sub_180008290 (DRAWBOT Supplier/Surface/Path/Draw + PF Effect Custom UI suites)
  has exactly one caller: sub_180001CB0 (the gated branch). **Unreachable.**
- `Color select` / `Enter new value` / `get_light_name()` ExtendScript: `Color select` is in
  sub_18000B3E0, which is `ae_ui::Ramp` widget code (vtable slot at 0x1800161F8, owner
  `??_7Ramp@ae_ui@@6B@`). No ramp parameter exists → **unreachable**. The JS dialog is the
  ramp value editor, not a colour-keep feature.
- Result: this binary has **no reachable custom UI**. Port the no-op EVENT and document it.

## 7. SMART_PRE_RENDER — sub_180001BC0

`extra` = PF_PreRenderExtra {input*@0, output*@8, cb*@16}.
1. `req` = copy 44 bytes of `*extra->input` (PF_RenderRequest = output_request).
2. `cb->checkout_layer(effect_ref, index=0, checkout_id=0, &req, in->current_time,
   in->time_step, in->time_scale, &result)` — **input only, no param checkout**.
3. `output->result_rect ∪= result.result_rect`, `output->max_result_rect ∪= result.max_result_rect`
   (sub_18000A740: copy if dst empty, else min/max union of non-empty rects).
4. No `flags`, `solid`, `pre_render_data` set. Returns the callback error.

## 8. SMART_RENDER — sub_1800010D0

`extra` = PF_SmartRenderExtra {input*@0, cb*@8}. `input` = PF_SmartRenderInput
(output_request 44 bytes, **bitdepth short @44**).
1. `cb->checkout_layer_pixels(effect_ref, 0, &in_world)` (cb+0), `cb->checkout_output(effect_ref, &out_world)` (cb+16).
2. colour buffer: `count` = checkout id 1 value (§4); colours id 2..count+1 via §5 into `ctx`.
3. Branch on `input->bitdepth` (cmp 8 / 0x10 / 0x20). Any other value returns `count` as the
   error code (binary quirk; never reached in practice).
4. Per branch: `in->utils->copy(effect_ref, in_world, out_world, NULL, NULL)` (copy at
   0x18000123E / 0x1800012F0 / 0x1800013AE; r15 zeroed at 0x18000110D) — **RGB and alpha preserved first**.
5. `PF_Iterate{8|16|Float}` over `rect = out_world->extent_hint` (out+44, PF_Rect LTRB ints),
   refcon = ctx, kernel below, dst = out_world, src = in_world.
   Iterate wrapper sub_1800074B0 / sub_1800075D0 / sub_1800076F0; progress rows = bottom−top+1.

ctx (refcon) layout, as written by sub_1800010D0 (rsp offsets re-based on refcon base R):
`R+32` = int count; colour i floats at `R+36+16i` = {A, R, G, B}; kernels read R,G,B at
`R+40+16i`, `R+44+16i`, `R+48+16i` (A at `R+36+16i` is **not compared**).

## 9. Matching kernel and math (pixel kernels)

Each kernel gets the input pixel and writes **one channel: the first byte/word/float
(= alpha, PF order A,R,G,B) of the output pixel**; RGB come from the copy in step 4.

Channel conversion to float:
- 8 bpc (sub_180007140): `R = (float)p[1] * 0.003921569f` (bits 0x3B808081), G=p[2], B=p[3]; A=p[0].
- 16 bpc (sub_1800071B0): `R = (float)p[1] * 0.000030517578f` (= 2^-15, bits 0x38000000); A=p[0].
- 32 bpc (sub_180007220): R = raw float bits of p[1] (no scale); A=p[0] raw.

Match (sub_1800015C0 / sub_180001690 / sub_180001780): for colour i in order,
`match ⇔ |col_i.R − R| ≤ 1e-4f ∧ |col_i.G − G| ≤ 1e-4f ∧ |col_i.B − B| ≤ 1e-4f`
(tolerance literal 0x38D1B717 = 1e-4f; comparisons are `> 1e-4f` = mismatch, NaN compares as match-false in `>`:
NaN gives `fabs(...) > eps` false → treated as a **match** for that channel).
- On first match: `out.alpha = in.alpha` (raw first channel).
- If no colour matches (or count ≤ 0): `out.alpha = 0`.
- **Alpha is not compared.** Colour alpha (f0) is ignored entirely.
- Output RGB = input RGB (from the copy) in all cases.

Old 1.0.1 compared all four channels with `(u8)(int)((c+1/510)*255)` quantisation. **Changed.**

## 10. Changes vs old 1.0.1 (old `ColorKeep.aex` (git 408cf40), 27 KB)

| Area | Old 1.0.1 | New 1.1.0 |
|---|---|---|
| Framework | plain SDK sample, `entryPointFunc` | MyEffect (`entry_point`), object in global handle |
| Display name (PiPL `name`) | `Color Keep` | `OLM Color Keep` (match name unchanged) |
| Version | PF_VERSION(1,0,1) | PF_VERSION(1,1,0) |
| About | `Color Keep v1.01` | `OLM Color Keep 1.1` / `Constraint colors` |
| Colours | 100 (ids 2..101), max 100 | **200 (ids 2..201)**, count valid 0..200 |
| Count param | SLIDER 0..100 dflt 1, SUPERVISE | SLIDER 0..200 (valid & slider), dflt 1, SUPERVISE |
| UI hide | UPDATE_PARAMS_UI hides ids ≥ count | same logic, 200 ids; **not requested by out_flags** (§11) |
| Render | RENDER broken no-op; SmartRender, input checked in | RENDER no-op; SmartRender copy-then-alpha-iterate |
| Match | all four channels, quantised 8/16/32 | **RGB only** (alpha ignored), float 1e-4 tolerance |
| Match on alpha | alpha must match | not compared; output alpha = input alpha when matched else 0 |
| PreRender | checkout + rect union | same (rect union; no flags) |
| EVENT / custom UI | none | Drawbot draw + Ramp JS, **unreachable** |
| AEGP | RegisterWithAEGP | RegisterWithAEGP + Compute Cache acquire (unused) |

## 11. Quirks and deviations to decide on in the port

- out_flags omit `SEND_UPDATE_PARAMS_UI`. The binary handles UPDATE_PARAMS_UI but never
  requests it, so AE may never hide unused colours. Port mirrors the binary (0x02000040)
  and the parent should decide whether to add 0x04000000 (old port did).
- count > 200 overruns the binary's buffers (UB). Port clamps to 0..200.
- count < 0 → no colours; alpha 0 everywhere.
- NaN colour components: `fabs(NaN) > eps` is false, so a NaN channel counts as a match.
- Unsupported bit depth returns `count` as PF_Err (binary quirk). Port returns PF_Err_NONE.
- Iterate rect comes from the output world's `extent_hint` (out+44), not the request rect.

# OLM Color Key: 2.3.1 -> 2.3.3 (binary dated 2026-11-20) change spec

Scope: diff of the new Windows binary against the 2.3.1 IDB already used for the Mac port.
Every fact below was read from IDA with an explicit instance id and an input-path assertion
inside the call (shared ida-mcp; "current" target is not trusted).

## Identity and method

| | Old (port baseline) | New (2026-11-20) |
|---|---|---|
| File (TEMP analysis copy) | `%TEMP%\opencode\OLMColorKey-analysis.aex` (173,056 B) | `%TEMP%\opencode\OLMColorKey-20261120-analysis.aex` (174,592 B) |
| IDA instance | `f9285a17388b` (`OLMColorKey-analysis.aex.i64`) | `e162dfee839d` (`OLMColorKey-20261120-analysis.aex`) |
| Functions | 609 | 618 |
| `entry_point` export | `0x180010010` | `0x180010680` |
| Sections (VA) | .text 0x180001000-0x18001F000, .rdata to 0x180029000, .rsrc 0x18002C000 | identical VA ranges; file offsets shift (raw .text 0x400 -> 0x400, .rdata raw 0x1DE00 -> 0x1E400, .rsrc raw 0x29C00 -> 0x2A200) |

Method: (1) mnemonic-hash and normalized-disassembly fingerprints for all functions;
(2) Hex-Rays dumps of every changed OLM function, diffed; (3) instruction skeletons with stack
offsets and struct displacements masked, then displacements checked pair-by-pair against the
layout map below; (4) ordered call-target lists mapped through the old->new identity map;
(5) data-constant bytes at every `.rdata` memory operand; (6) raw PE scans for the strings.

## Changed behaviour (verified)

### 1. Version
- PiPL `AE_Effect_Version` (`.rsrc`): `1148928` (`0x00118800`, `PF_VERSION(2,3,1,DEVELOP,0)`)
  -> `1153024` (`0x00119800`, `PF_VERSION(2,3,3,DEVELOP,0)`). Raw diff of the 0x358-byte
  `.rsrc` payload: exactly one byte differs, file offset 0x163 within the section (0x88 -> 0x98).
  All other PiPL bytes, including both out_flags words, are identical.
- GLOBAL_SETUP `my_version`: old `0x18000B31B  mov dword ptr [r8], 118800h`; new
  `0x18000B98B  mov dword ptr [r8], 119800h`. Its out_flags write (`2000440h`) is unchanged.
- ABOUT (`PF_Cmd_ABOUT`): old `0x18000FF60` calls `sprintf(return_msg,"%s %d.%d.%d\r%s",
  "OLM Color Key",2,3,1,...)`; new `0x1800105D0` passes bug=3. Text is now `OLM Color Key 2.3.3`.
- Bit check against `AE_Effect.h` (`PF_VERSION` at line 142, fields at lines 120-127): 2.3.1 ->
  `0x118800`, 2.3.3 -> `0x119800`.

### 2. Slot count: 25 -> 200 (the only user-visible feature change)
- Manual (new): "Number of Colors ... Maximum of 200 colors". Binary agrees.
- PARAMS_SETUP old `0x180001000` / new `0x180001000`:
  - Loop count: old 25 iterations (`v6=25`); new two loops of 100 (`v6=100`, then `v9=100`).
  - Numbers in names: loop 1 uses 1..100, loop 2 uses 101..200 (printf `%s %d` with the index).
  - `num_params` (`out_data+48`): old `224` (`mov dword ptr [r13+30h], 0E0h` at `0x180001876`),
    new `1624` (`mov dword ptr [rax+30h], 658h` at `0x180001BB9`) = 24 + 200*8.
  - "Number of Colors" (uu 21): `PF_ADD_SLIDER(0, valid_max, slider 0..30, default 1)`. Only
    valid_max changes: old `0x1900000000` (25), new `0xC800000000` (200). Slider 0..30 unchanged.
- Static parameters (positions 1..23, uu ids 1,2,3,4,5,522,6,7,8,9,10,11,12,13,14,20,16,17,18,19,
  16,21,523) are the same helpers, names, defaults and flags as old. Positions 0..223 (input +
  statics + slots 0..24) are identical to 2.3.1, so projects saved with 25 colours still bind.
- Slot block: 8 params per slot at position `24 + 8n`, in this order inside each slot:
  `k0` Use Color (bool, default 1), `k1` Use Replace (bool, 0), `k2` Color (colour, black),
  `k3` Replace Color (colour, black), `k4` Threshold, `k5` Threshold(R..), `k6` Threshold(G..),
  `k7` Threshold(B..) (floats 0..1, default 0). Same order as 2.3.1.
- uu.id scheme (binary-exact; table lookups in checkout/UI and ParamDef ids):

  | n | Use Color | Use Replace | Color | Replace Color | Threshold | Thr R | Thr G | Thr B |
  |---|---|---|---|---|---|---|---|---|
  | 0..99 | 524+3n | 525+3n | 22+5n | 526+3n | 23+5n | 24+5n | 25+5n | 26+5n |
  | 100..199 | 1024+3n | 1025+3n | 324+5n | 1026+3n | 325+5n | 326+5n | 327+5n | 328+5n |

  Evidence: loop 2 of setup (`v11=825`, `v12=1325` as bases), checkout branch `v9>=100`
  (`0x18000A8E0`, constants -1/-2/-300..-303, -499..-501), UI loop 2 (`v24=1325`, `v26=824`).
  Spot values: n=100 -> Color 824, Thr 825, Use Color 1324, Use Replace 1325, Replace 1326.
  The ids form one contiguous set 22..1623 with statics 1..21, 522, 523 (no collisions).
- The uu table (`a1+8`) now has 1624 entries; table-end pointers move `a1+904` -> `a1+6504`
  (224 -> 1624) in every param helper and in the UI/checkout id lookups (`cmp 0E0h` -> `658h`).

### 3. Checkout: `0x18000A370` -> `0x18000A8E0`
- Scalar parameters: same helper semantics and ids as old.
- Slot loop runs `k < count` (`a6[14]`), with the id scheme above (branch at k>=100). Count < 0:
  no slots, no error (same as old). Count > 200: slot k=200 asks for Use Color id 1624, which
  does not exist, so the lookup yields index -1 and the checkout fails (old: fails for > 25).
- Scratch struct grows: Replace Color 516+16k -> 3316+16k; Use Color 1316+k -> 9716+k; Use Replace
  1341+k -> 9916+k; Thr 916+4k -> 6516+4k; R 1016+4k -> 7316+4k; G 1116+4k -> 8116+4k;
  B 1216+4k -> 8916+4k; Color 116+16k unchanged. Scratch is ~10.1 KB (SmartRender frame
  0x5C0 -> 0x27F0, with `__alloca_probe` at `0x18001D260`).
- Epsilon tail (`931135488`=1/65536, `989888641`=1/510, `897988541`=1e-6) identical.

### 4. UI: `0x180001A50` -> `0x180001D90`
- Same truth table. Loop 1 covers slots 0..99 (`v20+3 < 825`); loop 2 covers 100..199
  (`v31+3 < 1625`), with the enable test `100+j < count` on the absolute index.
- Helpers: bool/float/int/popup reads `F0F0/F1B0/F150/F210` -> `F760/F820/F7C0/F880` (same
  semantics, table end 6504); `BD10` -> `C380` (table end 1624 entries); `BED0` -> `C540`
  (`a1+1626`; the stream-owner field moves `a1[676]` -> `a1[4876]`, layout only).
- Disabled flag 0x20 (PF_PUI_DISABLED) and AEGP HIDDEN writes unchanged.

### 5. Layout and entry
- Dispatch table (`entry_point`): cases 0..33 map to the same functions as 2.3.1 (verified per
  case; compiler EH layout differs, not behaviour). Cases 9-12, 16-21, 28, 32, 33 are
  `xor eax,eax; retn` in both binaries (no-op).
- Object: `memset` of `0xAF8` (2808 B) -> `0x4C98` (19608 B). Constructor `0x18000B1E0` ->
  `0x18000B850` changes only layout (`add rcx,0A98h -> 4C38h`, memset `700h -> 32C0h`).
- Vtable (`??_7MyEffect@@6B@`, new `0x18001FDC0`): +16 SMART_PRE_RENDER, +24 PARAMS_SETUP
  (`0x180001000`), +32 SMART_RENDER (`0x180001C20`), +40 nullsub, +48 UI (`0x180001D90`),
  +56 `0x18000BCC0`, +64 `0x18000BFB0`, +72..+96 `xor/ret` stubs.

## Verified unchanged (no port work needed)

- SMART_PRE_RENDER old `0x18000B560` -> new `0x18000BBD0` (same calls).
- SmartRender logic: old `0x1800018E0` -> new `0x180001C20`; identical except the frame and
  `__alloca_probe`; error helper `0x1800108C0` -> `0x180010F30`; cookie check `0x18001CBF0` ->
  `0x18001D2C0`. Kernel dispatch 8/16/32: old `0x180009440/8F90/98F0` -> new
  `0x1800099B0/9500/9E60`, all strong-equal.
- Keyers (the render math): old 8/16/32 `0x180001E00/0x1800029D0/0x1800035D0` -> new
  `0x180002360/0x180002F30/0x180003B30`. Instruction skeletons identical after masking
  displacements and stack names (only `sub rsp` 0x2A0->0xAD0, 0x290->0xAC0, and padding
  differ). All struct displacements map through the layout table above. Call sequences
  (15 calls each) identical through the identity map. All 19 `.rdata` float constants per
  keyer identical in value and order. No keyer bug fix was found.
- Edge pipeline, all strong-equal (skeleton) and with identical call targets and constants:
  chamfer Approximate `0x180005D60` -> `0x1800062D0`; Box `0x180006E20` -> `0x180007390`;
  blur profiles `0x1800085A0` -> `0x180008B10` (calls equal; the only differing bytes are
  `__TI1H` ThrowInfo RVAs shifted 8 bytes, `0x28148` -> `0x28150`); `0x1800056F0` -> `0x180005C60`;
  `0x1800049A0` -> `0x180004F10`; distances `0x180004190`->`0x180004700`, `0x180004510`->
  `0x180004A80`, `0x180004850`->`0x180004DC0`, `0x180004910`->`0x180004E80`,
  `0x1800043A0`->`0x180004910`; Lab `0x180009EE0`->`0x18000A450`; EDT FH
  `0x18000A710`->`0x18000AD80`; FH `0x18000A920`->`0x18000AF90`; chroma/sin helpers
  `0x180008A60`->`0x180008FD0`, `0x180008DD0`->`0x180009340`; union rect `0x1800140C0`->
  `0x180014730`.
- CRT / std / dllmain functions differ only by naming and layout (not OLM code).

## Target Source feature: not present (premise corrected)

The strings `Target Source Enable`, `Target Source Mode`, `Point-Color|Color-Color|Point-Layer`,
`Target Point`, `Target Color`, `Source Color`, `Select Layer` exist in BOTH binaries
(old `0x18001F560..0x18001F5E8`, new `0x18001F5A0..0x18001F628`).
- No code xrefs to them in either binary (IDA `XrefsTo` empty).
- No data pointers and no 32-bit RVAs to them in either raw file (byte scan of both `.aex`).
- `num_params` in the new binary is exactly 24 + 200*8 = 1624, so no extra parameters exist
  (PARAMS_SETUP has no Target group).
- The new manual (`scratchpad\manuals\OLMColorKey.txt`) documents no Target Source feature.
Conclusion: these are dead strings carried in 2.3.1 too. The port must not add these params.

## Known residuals / open points
- Count > 200 fails in the binary (see checkout). The port keeps an explicit guard at
  `OLMCK_MAX_COLORS` (200). Count -1 is not rejected (same as 2.3.1).
- Slot names: names are the binary's `%s %d` with n+1, which the port already uses.
- Scratch dumps used for this analysis are in `%TEMP%\opencode\olm-e2e\colorkey-20261120\analysis\`.

## Verification of the port (2.3.3)
- MinGW fake-host test (`OLMColorKey\test_color_key.cpp`, out-of-source exe in TEMP): PASS,
  4,061 checks (2.3.1 suite: 1,056). Adds: 200-slot UI states, uu.id bijection 22..1623,
  literal ids for slot 100 and 199, count 201 rejected, count 200 renders, version 1153024, About 2.3.3.
- E2E differential (`%TEMP%\opencode\olm-e2e\colorkey-20261120\`, reference = 2.3.3 `.aex`, port
  included in the same process): PARAMS 1623/1623 definitions identical, num_params 1624;
  about text and GLOBAL_SETUP `my_version` 0x119800 identical; UI 312/312 states with identical
  effective state and identical event multisets (order differs: binary interleaves HIDDEN and UI
  per slot, same as 2.3.1); render 5,475 runs, 33 divergent: 32 at input width 1 (the same class
  as the 2.3.1 baseline, binary out-of-bounds read) and 1 one-ulp libm difference (blur, 37x23,
  depth 32, max 2.98e-8). Pre-render rects 0/18 divergent; error codes equal; feature `count`
  sweeps (incl. count 101 boundary, slot 100 and slot 199 only) 81/81 identical.
- Not run: Mac `build-all.sh` / AppleClang compile and AE smoke test (main-agent / user side).

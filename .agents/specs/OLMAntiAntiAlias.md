# OLM Anti Anti Alias 1.1.6 - verified spec and port status

Binary (reference): .opencode/olm-opentools-windows/OLMAntiAntiAlias/OLMAntiAntiAlias.aex (sha256 c0348e4d...ec4e).
IDA working copy: %TEMP%\opencode\OLMAntiAntiAlias-analysis.aex (same hash). IDA instance 66f5a770d42f.
Pin instance_id on every execute_python call: the shared "current" target pointed at other agents' databases.
Decompiles: %TEMP%\opencode\specs\aaa-decomp\*.c (unfiltered Hex-Rays).

## Status (2026-11-20, final)
- Port: OLMAntiAntiAlias/ (six-file layout). MinGW -Wall -Wextra -fsyntax-only: exit 0.
- Unit test OLMAntiAntiAlias/test_anti_anti_alias.cpp: 30 checks pass.
- E2E harness %TEMP%\opencode\olm-e2e\antiantialias\e2e_antiantialias.cpp (real .aex staged beside it), 302 checks pass:
  - PARAMS_SETUP 107/107 identical (types, flags, ids, names, slider ranges, COLOR defaults, button labels).
  - GLOBAL_SETUP version/out_flags2 identical; out_flags differs only by the accepted SEND_UPDATE_PARAMS_UI deviation.
  - SMART_PRE_RENDER rects identical.
  - SMART_RENDER randomised: 5000 runs at 8/16/32 bpc, 0 differing bytes (full buffers incl. padding). Coverage: sizes
    1..64 x 1..64, padded rowbytes 0..3 px, partial output extents (random sub-rect), counts 0..100, use_auto on/off,
    palettes of 2..6 base colours with near-duplicates (+-1/255) and exact duplicates, Voronoi flats with anti-aliased
    seams (ring paths), gradients, alpha dropouts, downsample_x = 2 (kernels ignore it, both sides).
    Not covered: origin offsets beyond extent_hint (PF_EffectWorld has no origin field in the SDK).
  - UPDATE_PARAMS_UI and USER_CHANGED INIT/ADD/CLEAR: 294 cases (fixed + randomised arrangements, counts 0..100,
    8 and 32 bpc, all three patterns, colours beyond the count non-zero), 0 differing traces.

## Identity / PiPL (VA 0x1800140B0)
- kind eFKT; name "OLM Anti Anti Alias"; category "OLM Plug-ins"; match name "OLM OLM Anti Anti Alias" (doubled OLM).
- PiPL 2.0; spec 13.29; eVER 569344 = PF_VERSION(1,1,6,DEVELOP,0).
- out_flags 0x02000040; out_flags2 0x08001400. Port adds SEND_UPDATE_PARAMS_UI (accepted deviation, keep).

## Dispatch (entryPointFunc 0x1800085E0)
ABOUT 0. GLOBAL_SETUP 1. PARAMS_SETUP 4 -> 0x180005D80. RENDER 11 legacy (port: documented no-op).
USER_CHANGED 13 -> 0x180006BC0. UPDATE_PARAMS_UI 14 -> 0x1800069B0 -> 0x1800066F0. SMART_PRE_RENDER 23 (inline).
SMART_RENDER 24 -> 0x180006360 (dispatch on bitdepth).

## Render (SMART_RENDER) - verified by e2e
- Pre: utils copy; colours alpha forced 1.0; use_auto -> scan (9999 cap) appends colours.
- Pass-through: no colours, or one black colour.
- Pair table D[i+N*j] = C[j]-C[i], D2 = |D|^2 (sub_180009500). Maps A (exact), B (working), init -1.
- Pass 1 over output extent: alpha 0 copy; exact quantised match -> palette, A=B=k; else (1,0,0,0).
- Pass 2 over input dims: opaque unmatched -> ring r=1 then r=2 (sub_180003720). Candidates: 1 -> SinglePair
  (sub_18000AC80), >1 -> BestPair (sub_18000A9C0); first index -> B and palette; else own pixel.
  Partial alpha -> own pixel.
- Pass 3 over input dims: alpha==1 keep; B>=0 keep; alpha<=0.5 -> (0,1,1,1); else flood r=1..4 (sub_18000A330).
- Pair cost (sub_18000A610 / PairCost): tt = (P-C1).D / |D|^2. If tt is NaN (identical colours, 0/0), the cost is NaN
  and the binary never accepts it (comiss/jbe), so such pairs cannot be picked. Verified by the tiny duplicate-palette
  case and by the randomised failures (32/1000 before the rule, 0/5000 after). Port: PairCost returns NaN for tt NaN.

## USER_CHANGED (0x180006BC0) - verified by e2e (294 cases)
- Clear (5): colour streams 7..106 <- (0,0,0,0) doubles (alpha,R,G,B) at AEGP_StreamValue2.val+8.
- INIT (3): clear (0x180005B00) then add-mode with ALL 100 slots free in order, used = 0.
- ADD (4): compaction: used = non-black colours in range + 1 for the FIRST black colour in range (it is not a
  free slot). Free slots = other black colours in range (ascending) + every slot >= count (ascending).
- Scan: upstream render (LRO v2 NewFromUpstream, SetDownsampleFactor(1,1), RenderSuite RenderAndCheckout, GetReceiptWorld,
  WorldSuite3 FillOutPF, PF World v2 GetPixelFormat). Scan result capped at free-slot count; dedupe against the existing
  colours (count entries, alpha included).
- Write order: RENDER -> VAL count (one_d, first 8 bytes; binary leaves stale bytes after it) -> VAL colour streams ->
  CHECKIN -> LRO_DISPOSE -> EFF_DISPOSE.
- Count rule: total = used + found; count is written as total only when total > count (INIT: used = 0).
- Previous residuals (slot choice, INIT found count) were count/slot rules, not scan differences: resolved.

## UPDATE_PARAMS_UI (0x1800066F0) - verified by e2e
For ids 7..106: Stream v7 [5] GetNewEffectStreamByIndex, Dynamic v3 [4] GetDynamicStreamFlags, Dynamic v3 [5]
SetDynamicStreamFlag(HIDDEN=2, undoable=0, set = i >= count). Effect v3 [8] DisposeEffect.

## Legacy RENDER (case 11): binary has a legacy path (0x1800089B0 + 0x180001300/0x1800014C0). Port: documented no-op.

## Open items
1. Legacy RENDER no-op (deviation, accepted).
2. Origin offsets beyond extent_hint not exercised (SDK has no origin field).
3. Mac compile and AE smoke test (main agent: build-all.sh).

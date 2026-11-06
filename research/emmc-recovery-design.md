# eMMC -2 root cause + recovery design (loop-10, Nov 6)

## Facts
- -2 = DATA_INHIBIT stuck >50ms (op2_emmc.c:236-239), controller RESET_DATA,
  return stuck. Comment already notes "-2 never recovers by retry".
- Loop-10: 4953 fails, FAILRING all SAME LBA (18246624) rc=-2. Same LBA reads
  FINE in TWRP/Linux. 1.45M other reads OK in same boot.
- Card clock 25 MHz (margin huge); XNU SKIPS re-init when card already TRAN
  (inherits shim state); shim does CMD0 once.

## Root cause (best fit)
Card-side wedge (DAT0 busy / open transfer from shim residue or a stuck
write), NOT media, NOT signal. Our -2 path resets only the CONTROLLER;
the card stays wedged, so every subsequent data command re-wedges:
reset-abort self-perpetuating stall on the affected region. Linux recovers
(CMD12 abort), we never send it. TWRP-immune explained.

## Fix (loop-12 kernel, all in op2_emmc.c)
1. -2 path: after RESET_DATA, send CMD12 (STOP, best-effort), CMD13 (status);
   if still non-TRAN/busy, CMD0 (0xF0F0F0F0) + re-select CMD7 to TRAN.
2. XNU init: ALWAYS full re-init (drop the TRAN early-return); deterministic
   card state, flushes shim residue. Cost ~100ms boot.
3. DATA_INHIBIT timeout 50ms -> 500ms (covers GC spikes; fewer spurious -2).

## Validation
Clean roll + targeted -2 injection is hard; validate statistically: 3+
consecutive clean rolls (vs current ~67% bad streak) + zero FAILRING spread.

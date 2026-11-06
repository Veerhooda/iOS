# hunkCV: SpringBoard min38 SEGV (loop-22/run169) — root cause + guard

## Crash
- SB pid41 SIGSEGV at min38-39 (PROC_EXIT line 768, between TL min38/min39).
- pc=0x1e8f65c60 (CoreFoundation +0x77c60), far=0x0, x0=0, lr=0x1ec350fe0
  (CoreVideo +0x18fe0). Slide 0x68140000.
- Fault insn `[pc]=0xf9400008` = `ldr x8, [x0]` (CF prologue reads isa of
  NULL object). x1 = 0x1ec351094 = CV static 0x184211094 (a function
  pointer = CF apply-function callback).
- Presents froze (6698->6884 then flat) and panel went nz 2000->0 at the
  same minute: SB's death IS the black freeze (backboardd alone can't
  render UI). SB respawned (pid201, idle in GSEventRunModal) but never
  re-rendered.

## Call chain (ESTK #114 + disasm)
SB main +0x443298 -> CoreVideo 0x184210edc(x0, x1=NULL-dict) ->
  0x184210fd8: mov x0, x20 (x20 = x1 arg, unchecked)
  0x184210fdc: bl CFDictionary/SetApplyFunction (0x180e25c48)
  -> SEGV reading NULL dict.

Likely a no-GPU NULL: a CV object that never gets built without Metal is
passed (or its NULL attrs) where a live collection is expected.

## Guard (tools/hunkCV.py, 1 slot, no trampoline)
At static 0x184210fd8 (file 0x4210FD8):
  mov x0, x20 (0xaa1403e0) -> cbz x20, #8 (0xb4000054)
Safe because: the out-param [sp,#0x10] is pre-zeroed (str xzr @0x184210f40),
the bl's w0 result is ignored (fall-through only reads the stack slot), and
the fall-through already bails gracefully on NULL
(ldr x20,[sp,#0x10]; cbz x20 -> epilogue @0x184211078).
Skipping "apply fn to NULL collection" = vacuous no-op. Verified: the `bl`
word decodes to exactly 0x180e25c48; site bytes asserted before writing.

## Symbolication pipeline (rebuilt 2026-11-06)
- tools/op2slide.py <log> -> run slide (dual-anchor).
- tools/op2sym.py <cache> <slide> <pcs...> [--exec base] -> dylib+off.
  Validated on 6 known run168 frames.
- tools/disas_cache.py <staticVA> <size> (CACHE_PATH now /tmp/fixaro mount).
  NOTE: assumes file_off = VA-0x180000000 (first mapping only); verify by
  decoding a known branch target before trusting new regions.
- tools/trie.py parses export tries but extracted dylibs lack LINKEDIT
  (dyld_extract drops it) -> "no symbols"/"no export trie". For names, use
  otool -oV (ObjC methods) or parse tries from the cache image directly.
- tools/dyld_extract.py <cache> extract <substr> <out> needs a UNIQUE
  substring (e.g. 'Frameworks/CoreFoundation.framework/CoreFoundation').

## Loop-23 (run170)
fixA + hunkQC + hunkCV + lskdd-parked, quiet periodic tap logs, 7200.
Booted 14:29, reset 16:30.
- SB pid41 NEVER DIED (hunkCV held; 0 SB deaths). But server never left
  surface 2 in 112 min (presents 2468, steady 24/min BootUI animation).
- SB main blocked in SYNC MobileAsset XPC (AccessibilityUtilities ->
  MobileAsset -> libxpc -> libdispatch -> mach_msg SEND, no timeouts):
  mobileassetd never died but never replies (wedged on eMMC or self-
  deadlock). Hang is infinite (112 min).
- eMMC: 2.26M reads, 6 rd + 10 wr fails, 16 recoveries, retry_ok=0.
  Writes are single-shot (no retry): wr_fail = EIO to HFS (journal risk).
  TODO: write-retry hardening (reliability; 10 lost writes/2h rots FS).
- run170 slide via mach_msg_trap anchor (op2slide needs crashes):
  trap static 0x180a580f4 (from run168), slide = trap_pc - static.
  SB/bb trap pcs identical (0x1b0ee40f4) -> slide 0x3048c000 confirmed.

## Loop-24 (run171)
trim7 (mobileassetd parked) + same kernel. IDENTICAL to loop-23 (SB in the
same MobileAsset XPC wait, trap-relative offsets match exactly). Parking
does NOT fail fast: launchd holds the message for the never-coming server
and SB (no XPC timeout) waits forever. Park was wrong; daemon must be
ALIVE to reply.
- eMMC: all 6 fails rc=-2 (CMD response timeout). read_retry never
  covered -2 (only -3/-4) -> every -2 was instant-EIO. Writes single-shot.
- Daemon-health ⟺ eMMC-luck correlation: 0-1 fails (run168/169) = daemon
  replies, SB proceeds; 6-16 fails (run170/171) = daemon wedged/silent.

## Loop-25 (run172)
Kernel: read_retry covers -2; new write_retry (-2/-3/-4, 3x) + wr_retry_ok
STATS field; SNAP covers mobileassetd. trim8 = fixA + QC + CV + lskdd only
(mobileassetd RESTORED). Predicts: ops survive -2 via abort+reissue,
daemon inits cleanly and replies, SB drives past BootUI to UI.
Fallback (if hang persists): MA client fail-fast patch (audited MA
0x187aed9c4 wrapper + 0x187aeabc8 caller; both normal AND error paths
deref the reply, so patch must be higher or forge XPC_ERROR; see chat).

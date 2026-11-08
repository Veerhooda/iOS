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

## Loop-27 (run174): hang returns, daemon exonerated, hunkMA designed
- trim9 + print-budget kernel: full TL kept (107 lines min6-112!), eMMC
  0 fails with 15 saves (retry_ok=3, wr_retry_ok=12). assetsd gone (0).
- BUT surface 2 again (2,472 presents): SB at the IDENTICAL MA XPC wait
  (trap-relative 0x7095900, 3rd run). Budget is log-only; the trigger is
  the assetsd park OR input timing (query is input/boot race).
- mobileassetd pid112 HEALTHY (main runloop idle, 6 threads): the message
  never arrives (launchd holds it; daemon never got it). Hang is in the
  delivery/wait, not daemon health. Unpark/restore cannot fix it.
- hunkMA (tools/hunkMA.py): skip `[MAAssetQuery runQueryAndReturnError:]`
  in AXUtils (selector confirmed via methname cstring). Site AX+0x6030:
  mov x0,x20 + bl -> mov x0,xzr + nop. Out-param pre-zeroed; post-call
  retains nil and takes the existing cbz graceful path (pure ObjC
  nil-messaging, audited through 0x190b8619c). "Found nothing" = true.
- Loop-28: trim10 = trim9 + hunkMA (+ AT pref if pull confirms it).

## AssistiveTouch bake, done properly (loop-28/trim10)
- Old attempt (data_ark.plist, 5 copies) was a guess; new one is grounded:
  pulled the live 7.2GB userdata image and read what iOS itself wrote.
- Ground truth: /var/mobile/Library/Preferences/com.apple.Accessibility.plist
  (mobile, mode 600), plain key `AssistiveTouchEnabled` as INTEGER
  (NOT the AXS* strings: those are notifyd keys like
  _kAXSAssistiveTouchEnabledNotification, not plist keys).
- Live file had all-0 defaults (user never re-toggled in loop-27); baked
  file = the live system-written file with AssistiveTouchEnabled 0->1
  (integer, via plistlib), plutil -lint OK, chmod 600 (uid already 501).
- fixA lacks the file (created on first boot) -> bake CREATES it.
- Failure mode if wrong: AT stays off (status quo); a data-only plist
  cannot break boot. Fallback: reflash trim9 (rebuild from fixA).

## Loop-28 (run175): hunkMA WORKED, next hang in same MA trap (TTS path)
- trim10+hunkMA on device (verified bytes: QC/CV/MA all patched): SB main
  NO LONGER in the AX path. run174 stack: AX+0x6038 -> AS runQuery...;
  run175 stack: TextToSpeech -> 0x1098?? -> AS runQueryAndReturnError:+0x14
  -> runQueryForInfoReturnError:+0x9c -> libxpc+0xa850 sync -> mach_msg.
  Same trap, different caller. 24 one-shot deaths, eMMC 0 fails
  (retry_ok=4, wr_retry_ok=5), 2471 presents surface-2, panel lit,
  AUTOHOME/AUTOSWIPE 46 each (kernel-side fine; UI parked so no effect),
  assistivetouchd launched (plist AssistiveTouchEnabled=1 on device).
- Lesson: call-site whack-a-mole cannot win; patch MA itself (hunkMA2).
- Park method ground-truthed: LaunchDaemons plist DELETED (binaries stay).

## hunkMA2 (tools/hunkMA2.py): stub 4 sync-query entries in MobileAsset
- Sites (static VA, 2 slots each -> mov x0,xzr; ret):
  AS runQueryAndReturnError:  0x187ae86e8 '@'->nil (observed run174+175)
  AS runQueryForInfoReturnError: 0x187ae8414 '@'->nil (inner frame)
  MA queryMetaDataSync        0x187ae5fbc 'q'->0 (sync xpc at +0x210)
  MA queryInstalledAssetIds   0x187ae5c88 'q'->0 (sync xpc at +0x100)
- Completeness: scanned all MA __text direct-bl to libxpc; 0x180b1c850 is
  the sync primitive (all *Sync methods + our hang call it). Download/purge
  Sync methods left alone (not boot-path).
- Applied SURGICALLY on device (tools/op2hfs.py HFS+/HFSX reader/writer;
  cache contiguous 1 ext @blk158603): 8B/site, read-back verified,
  neighbors intact. Zero rebuild/reflash risk; trim10 otherwise verbatim.
- Loop-29: same kernel/cmdline, 2h; predicts SB past BootUI to UI.

## Loop-29 (run176): hunkMA2 UNBLOCKS to full UI; dataaccessd storm floods ring
- 38,390 presents surface-9 lit; user drove real apps (Photos, Settings).
- eMMC 1.9M reads 0 fails (3 saves). SB pid41 alive.
- dataaccessd SIGABRT x311 + cs_invalid_page x326 (pages misattributed by
  op2sym to libswiftsimd+0x94M -- actually main-executable range; NOT the
  hunk sites). Abort pc libsystem_kernel+0x230dc via pthread+0x2094.
  Storm evicted Photos/Settings deaths from the 256-slot ring (771 total).
- Fix: exit ring follows the print-budget verdict (first 24/comm recorded,
  overflow counted as dropped; crash state still consumed). Loop-30 with
  this kernel to capture Photos/Settings sites while user drives.

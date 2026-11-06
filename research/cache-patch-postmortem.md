# Cache-patch postmortem (run99, 2026-11-03)

## Proven: 2 of 3 dyld_shared_cache hunks in tools/mkop2root.py were wrong

Image inspected: out/ios12-userdata-fixA.dmg (mounted read-only on macOS).

### Hunk 1 (0xdb0d88, CoreFoundation CFRelease): HANG, not safe-return
- Container verified: CoreFoundation @0x180dae000 + 0x2d88 (image table walk).
- Patch replaced `cbz x0, +0x68` (NULL -> trap path) with `cbz x0, #0`.
- A64 `CBZ Xt, #0` branches to ITSELF: any CFRelease(NULL) spins forever.
- On single-CPU OP2 a spinner eats the only core and wedges the caller.
- Dormant unless something releases NULL (no SIGTRAP deaths seen, so likely dormant).

### Hunk 2 (0xdb0e04, CFRelease brk #1 -> ret): masks fatal NULL release
- Returning mid-CFRelease with inconsistent state corrupts the caller.
- Reverted to stock so a NULL release faults visibly (SIGTRAP) instead.

### Hunk 3 (0x2ee0d714, believed MediaPlayer setRoutingContextUID:): WRONG DYLIB
- VA 0x1aee0d714 belongs to libswiftFoundation.dylib @0x1aed60000 (+0xad714).
- Real MediaPlayer.framework is at 0x191e0a000, ~200MB away.
- Stale offset + coincidental `cbz x21` byte-match corrupted Swift code:
  x21==0 now jumps +0x4FC into a foreign epilogue.
- suggestd/ndoagent/itunescloudd link NO Swift (otool -L, transitive check for
  ndoagent is Foundation-only), so hunk 3 does NOT explain their aborts.
  It remains capable of crashing Swift users (SpringBoard links MediaPlayerUI,
  check transitively if needed). Reverted regardless: proven misplacement.

### SB/MAD patches: shapes verified sane (kept)
- SpringBoard isInSetupMode/brickedDevice/lockdownState + mobileactivationd
  tbz-nops/pointer swap all show correct patched bytes at plausible prologues.

## Decision
patch_shared_cache() DISABLED in apply_tweaks. Next image build ships STOCK
cache bytes. Device-side 4-byte dd-reverts planned instead of full reflash.

## Open: abort epidemic cause (NSInvalidArgumentException, many daemons)
- On-device logd store contains real substituted 'NSInvalidArgumentException'
  messages. .ips absent (ReportCrash never spawns). Tracev3 pull in progress.

## Correction (run130, 2026-11-04): hunk 1 was CORRECT, not a hang
- Re-examined encoding: bytes `40 00 00 b4` = u32 0xB4000040, CBZ imm19 =
  (0xB4000040>>5)&0x7FFFF = 2, target = pc+8 = 0xdb0d90, NOT self.
  The run99 "CBZ Xt,#0 branches to ITSELF" claim misread imm19 (2, not 0).
- 0xdb0d90 holds a genuine early-out `ret` (c0035fd6) with nothing pushed,
  so CFRelease(NULL) returns safely and stack-balanced.
- Empirical: with hunk 1 (runs 122-129) SpringBoard was quiet; with stock
  bytes (run130) SpringBoard SIGTRAPs 22x/run at the 0xdb0e04 brk (x0=0),
  called from libARI `_ARI_CsiIceAtExtReq_ENC+0x7c` (baseband AT path with
  no modem => NULL release). So hunk 1 is LOAD-BEARING, not dormant.
- Fix applied: fixA reflashed with hunk 1 ONLY (hunk 2 reverted to stock brk
  so non-NULL misuse stays visible; hunk 3 reverted, was real corruption).
- mkop2root.py updated: patch_shared_cache() re-enabled with hunks 2+3 removed.

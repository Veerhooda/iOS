# Abort epidemic root-cause hunt (run99-run101, 2026-11-03)

## Method: on-device logd store beats ramoops
Pulled the 6.4GB userdata partition (adb exec-out dd+gzip), mounted on macOS,
mined /private/var/db/diagnostics/Persist/*.tracev3 with strings. No .ips
files exist (ReportCrash never spawns), but the "*** Terminating app" messages
with substituted args ARE in tracev3.

## Two signatures (76 tracev3 files, ~35 termination entries)

### Crash A (30+ hits): NSDictionary nil objects[4]
`NSInvalidArgumentException: -[__NSPlaceholderDictionary
initWithObjects:forKeys:count:]: attempt to insert nil object from objects[4]`
- Same index [4] every time => same-shaped 5+-key dictionary literal(s).
- run101 ESTK-24 (cache slide 0x11AD7E08, trap pc 0x19253b0dc):
  - SpringBoard pid=41 (#17): SB-exec -> UIKitCore+0x31341c -> ControlCenterUI
    +0x179c0 -> libdispatch -> libobjc(objc_exception_throw) -> libc++abi
    (__cxa_throw) -> abort. Worker thread (dispatch block in CCUI).
  - SpringBoard pid=112 (#22): IDENTICAL unslid chain (exec slide differs by
    0xAD2000) => deterministic per-launch crash site.
  - suggestd pid=111 (#18): same throw path, caller is
    CoreSuggestionsInternals+0x16e714.
- So Crash A is a CLASS (dict-with-nil in dispatch blocks), not one site:
  CCUI (SB), CSI (suggestd), presumably others for ndoagent/itunescloudd.
- fp-chain skips frameless init/helper frames; exact literal site needs the
  "First throw call stack" frame1 (tracev3, chunked) or exec slides + disasm.
- Neighbor clue (file c): Siri context-service XPC error just before one
  A-crash; victims (SB/ndoagent/suggestd/itunescloudd) are all
  account/identity-adjacent, but missing-AppleID alone is handled on stock,
  so the nil is OP2-specific (hardware/service/XPC/pref value).

### Crash B (4 hits): routingContextUID nil assertion
`NSInternalInconsistencyException: Invalid parameter not satisfying:
routingContextUID` (i.e. -[MPAVEndpointRoutingDataSource
setRoutingContextUID:] with nil).
- THE bug the prior agent tried to patch (hunk 3) and missed by ~200MB
  (landed in libswiftFoundation). Real site is in MediaPlayer @0x191e0a000.
- Nearby: SpringBoard x4, `-[WFClient _setupDefaultInterfaceWithManager:]`.
- run101: no B-crash caught (only 3 SIGABRTs, all A-shape). Rarer than A.

## Crash census notes (run101, 600s, ESTK-24)
- 267 deaths; 3 SIGABRT (2x SB, 1x suggestd, all Crash-A shape);
  16 SIGSEGV/SIGBUS (logd x3, SB x1, searchd, medialibraryd, ...);
  219 SIGHUP with comm=launchd, identical stacks, distinct PIDs (2nd-gen
  PIDs late in run; comm inherited pre-exec? spawn failures? UNDERSTOOD
  POORLY - parked, not UI-blocking).
- Suggestd also exits cleanly with status 4 (7x in run100) - idle-exit
  cycling, benign.
- Abort timing is chaotic run-to-run (0 in run96/98/100, 18 in run99):
  boot-phase-dependent; 600s windows catch them reliably.

## Next
run102 (600s, snaps on): snapshot dyld lines now carry pid+exec
(imageLoadAddress) so SB-exec ESTK frames unslide to SB file offsets;
then disassemble SB->UIKitCore->CCUI hop chain to the dict literal and
identify the nil expression. Then seed/patch the SOURCE (service, pref,
file, or XPC reply), not the cache.

## Breakthrough: ndoagent root-caused + FIXED (run102-run103)
- ndoagent chain: ndo-exec -> CF dictionaryWithObjects:forKeys:count:
  -> placeholder init -> throw. BL-test derived exec slide, otool
  (unstripped!) named the caller: -[NSUserDefaults ndo_registerDefaults],
  which calls MGCopyAnswer(@"SerialNumber") -> objects[4].
- MG SerialNumber was nil (no DT serial-number, no syscfg on OP2).
- Fix: tools/mkadt.py DT root now ships synthetic literals serial-number,
  mlb-serial-number, region-info, model-number, regulatory-model-number
  (real N56AP has these as iBoot-resolved syscfg refs).
- run103 verdict: ndoagent SIGABRT 17 -> 0, region-info MG errors gone.
  SB (4), itunescloudd (8), suggestd (1) still abort, SAME unslid sites
  (SB: CCUI+0x177c0/UIKitCore+0x31321c; SG: CSI+0x16e514) => their nils
  are non-MG sources. 36 -> 13 aborts/run.

## Next
run104: exit ring now captures exec (dyld infoArray[0]) at crash time in
sleh.c, so EVERY crash's 0x100 frames unslide without snapshot lottery.
Then walk SB-exec -> UIKitCore -> CCUI -> dict site with symbols and
identify the nil. Same for itunescloudd-exec and suggestd/CSI.

## Decode wins (run104-105)
- Exit-ring exec capture works: SB pid=41 exec=0x10081c000 etc. SB crash
  frames unslide identically across pids: SB 0x606300/0x605e08/0x3f8d24/
  0x3f877c/0x55ffc/0x478644. otool (SB unstripped!) names the path:
  +[SBControlCenterController sharedInstance] -> init ->
  initWithScreen (debugName "ControlCenterWindow") ->
  setRootViewController:(CCUIModularControlCenterOverlayViewController) ->
  view load/layout (_setHostsLayoutEngine:) -> CCUI -> dict -> BOOM.
- Dyld-cache __DATA pointers carry tags in bits 41-42: VA = stored &
  0x1FFFFFFFFFF. With this, ObjC metadata walks WORK (class -> ro ->
  methods -> IMP). Found: __NSPlaceholderDictionary
  initWithObjects:forKeys:count: IMP = 0x180DC8738; its nil-objects
  throw is bl @0x180DC8894 (format 'attempt to insert nil object').
  CCUIModuleInstanceManager._updateModuleInstances uses
  bs_setSafeObject:forKey: (nil-safe, NOT the site).
- CCUI (65 methods scanned) never calls dictionaryWithObjects directly;
  the crashing literal is in a skipped frameless callee (likely a tiny
  dispatch-block invoke). NOFILE namei logger abandoned (vfs TU drops
  OP2 code for unknown build reasons; reverted).
- Pending: newest tracev3 "First throw call stack" frame1 + known
  run104/105 slides => exact dict site.
- PROVEN METHOD (device tracev3 full stack, slide 0x41dd4000 via
  libdyld+0x18d8 anchor): f2=0x180E40BEC (_CFThrowFormattedException's
  objc_throw call), f3=0x180DC8898 (init nil-branch), f4=0x180DB9474
  (dictionaryWithObjects), f5/f6 unslide with exec slide 0x1294000 to
  ndoagent 0x100003BE4/0x1000037B4 = registerDefaults EXACTLY.
  Unwinder frame order: [unwind, objc_throw, CFThrow, init, dict-ctor,
  SITE-caller, ...]. Apply to SB/suggestd/itunescloudd stacks next.

# Device test plan — Fix A (GPU bundles intact) + Fix B (handoff diagnostics)

Date: 2026-11-03. Context: `research/quartzcore-simcrash.md` (root-cause
analysis), `research/digest-*.md` (full inheritance research).

## What was built (all verified locally)

| Artifact | Change vs run81 | Verify |
|---|---|---|
| `out/xnu-boot.img` (11.7 MB) | Kernel rebuilt: Fix B rate-limited `present #%` / `no target` / `nz~` diagnostics; removed orphaned `op2_fb_swaps_logged` that broke the build (`-Werror`) | Full XNU build, 0 errors; new strings confirmed in linked kernel |
| `out/op2-hardware-payload.bin` | Cmdline mirrors run66 + `op2_deadline=240` (exactly 255 chars, no truncation) | Header decode: magic `OP2XNU01`, cmdline length asserted |
| `out/ios12-userdata-fixA.dmg` (6.4 GB) | Byte-identical to run81's image EXCEPT `AGXGLDriver.bundle` + `AGXMetalA8.bundle` renamed back from `.disabled` (metadata-preserving `mv`) | `HFSX` magic at offset 1024; bundle contents listed |
| `tools/mkop2root.py` | `tweak_graphics()` no longer renames GPU bundles (self-heals old roots) | `tools/test_tweak_graphics.py` — 3 checks PASS |
| `tools/flash_userdata.py` | Optional image-path argv (default unchanged) | py_compile + override check |

NOT included in the fixA image: the uncommitted `mkop2root.py` pivot
(`CA_BACKDROP_SCALE`, backboardd-patch removal, hide-logo flip). This keeps the
test attribution-clean: fixA differs from run81's image ONLY by the bundle
restore. The pivot can be rebuilt/tested separately afterwards.

## Run 82 — procedure (needs the phone in TWRP + adb)

```sh
# 1. Flash the fixA image (streams ~6.4 GB; keep the phone awake)
python3 tools/flash_userdata.py out/ios12-userdata-fixA.dmg

# 2. Boot the new kernel (nothing is flashed to boot partitions)
fastboot boot out/xnu-boot.img

# 3. Watch the screen. The bar should animate 0 -> 100% over ~20 s (kernel
#    animator), then ONE of the outcomes below. The phone WILL auto-reset to
#    TWRP at the 240 s software deadline — wait for it (do NOT pull battery).

# 4. After it returns to TWRP, pull the log and save as logs/run82_ramoops.txt
adb pull /sys/fs/pstore/console-ramoops-0 logs/run82_ramoops.txt
```

## Outcome decision tree (read the screen first, then the log)

**Outcome 1 — screen leaves the logo and shows UI (Lock Screen / Setup / Home).**
Fix A confirmed: the `.disabled` renames were pushing iOS 12 QuartzCore into
the abort path. Next: stabilize (rebuild a clean `ios12-userdata.dmg` from the
fixed script, reflash, longer deadline run) and bisect the mkop2root pivot
separately if still wanted.

**Outcome 2 — still stuck at 100%, but `Simulating crash!` is GONE from the log.**
Bundles changed CA's path but compositing still fails. Look at the new lines:
`OP2 FB: present #N surf <id> (...) nz~<n>` — if `nz~` is large (>1000),
userland IS rendering and the kernel copy/panel kick is suspect; if `nz~0`
everywhere, CA still produces blanks (phase 2: shared-cache or accelerator
work, guided by the exact new log).

**Outcome 3 — still stuck, `Simulating crash!` STILL present (even once).**
Fix A falsified as the sole cause: with bundles intact, iOS 12 CA still refuses
software compositing. The decisive data is then:
`present #` target ids + `nz~` values + any `no target` lines. Send
`logs/run82_ramoops.txt` back — phase 2 is picked from that evidence (options,
in order: fake `AGXAccelerator` service probe; `0x1852fff98`-check shared-cache
patch; `fSurface` 24bpp/3240 alignment), NOT more guessing.

**Outcome 4 — black screen instead of logo (progress!).**
Means `present()` now copies userland frames (unconditional handoff works) but
frames are blank. Same log analysis as Outcome 2/3.

## Log checklist (grep the pulled ramoops)

- `Simulating crash` count (was 1 in run61/run66).
- `DISPLAY SWITCHED` / `HAND-OFF` present? (never observed before — first sighting matters)
- `present #` early lines: target ids, `nz~` values.
- `no target` lines? (would implicate swap-record parsing, not CA)
- `Code=26` count (FS damage from unclean-shutdown accumulation; if high,
  rebuild a CLEAN image from the fixed script before the next run — every
  deadline panic is an unclean shutdown on a journaled volume).
- Tail: `software timer deadline exceeded (240 s)` + reset to recovery (expected).

## Outcomes (executed 2026-11-03, device in hand)

### Run 82 — Fix A test (bundles intact, Fix B diagnostics, deadline=240)
Outcome 3: `Simulating crash!` still fired once; `surface 3 nz~0`. Fix A
falsified as the sole cause — but the run closed the biggest evidence gap:
first-ever observation of `DISPLAY SWITCHED`/`HAND-OFF`, and proof that
backboardd rasterizes on CPU (surface 2 lock/unlock seeds 1->5) while the
server composite stays blank. Full 162 KB log, head preserved:
`logs/run82_ramoops.txt`.

### Run 83 — GPU-less client-UI policy (`op2fb_client_ui=1`)
Kernel shows the fullest client surface when the server target is blank.
Result: `now showing surface 2 (swap target 3)`, `shown 2 nz~5001` on all 4
logged presents — real userland-rendered frames reach the panel. The frozen
kernel splash is replaced by actual iOS UI. Log: `logs/run83_ramoops.txt`.

### Next wall (not a display problem anymore)
- Render server wedges after 7 swaps (backboardd unsubscribes vsync, silence).
- SpringBoard never renders in ANY run (0 surface/FB ops); its AX thread parks
  on "Waiting for notification to start AXSB server" (also seen at 600 s in
  run77/78, so not timing).

### AXSB hunt outcome (2026-11-03, static analysis, no device cycle needed)
- Notification name confirmed: `AXSystemAppServerReadyNotification`
  (NSString bytes at cache VA `0x1bdec863f`).
- Exactly TWO images import it: `AXSpringBoardServerInstance` (the waiter —
  state check at `0x1a2b4c148` via libAccessibility bool-config helper) and
  `AccessibilityUIService` (second observer via `CFNotificationCenterAddObserver`,
  call site `0x19ba9cf70`, Coalesce).
- NO poster exists anywhere: no second copy of the name bytes in any binary,
  no GOT use besides the two observers. The notification is observed but
  never posted — because the poster is SpringBoard's own main thread after
  IT finishes launching, and IT is stuck pre-first-frame (0 FB ops; run80
  shows it subscribing to vsync then going silent).
- CONCLUSION: the AXSB wait is a SYMPTOM (side-thread gate), not the cause.
  Spoofing/patching it would start the AX server while SpringBoard main stays
  stuck — no UI. The real gate is SpringBoard main thread parked before its
  first CA commit, almost certainly on the dead render server.
- backboardd main thread is ALIVE late into the run (run83 lines 1994-2087:
  Home-button down/up events processed!). Only its render thread is dead.
  NOTE: those button events are either the user pressing buttons or ghost
  events from the digitizer driver — worth one explicit check.
- Proposed next step: capture SpringBoard main-thread user stacks at the
  deadline panic (kernel thread-dump aid), which names the exact parked call
  and unblocks everything downstream. Alternative: pivot to a custom
  client-rendered UI on the proven-working primitives (client surfaces +
  client-UI policy + input), bypassing SpringBoard.
- All userland binary patches audited PRESENT in the flashed image
  (backboardd finishedStartup + overlay removal, SpringBoard x3, shared
  cache x3) — no silent skips.

## run85-87: main-thread stacks name the hang (XPC chain, not render wedge)

- Kernel snapshot aid (`osfmk/arm64/op2_snapshot.c`): per-thread
  pc/lr/sp/fp/x0-x5/x16, 16-frame fp-chains, dyld slides, thread state.
- run85: SpringBoard main parked in infinite mach_msg receive (port 0xe03,
  opts 0x400400e); backboardd main likewise. NOT wedged mid-syscall.
- run86 (slide solved via trap-stub bytes, 0x5241C008): main is in a SYNC
  NSXPC call: mach_msg <- dispatch <- libxpc+0xA914
  (`xpc_connection_send_message_with_reply_sync`) <- Foundation
  (`_sendInvocation:...`, `_remoteInterface`) <- CoreServices/LaunchServices
  (`synchronousXPCProxyWithErrorHandler:` +
  `getServerStoreWithCompletionHandler:` + `com.apple.lsd.mapdb` strings).
  SpringBoard awaits lsd's reply (LS database server store).
- run87: lsd worker T739 is ITSELF in the same sync-XPC path via
  MobileInstallation (`enumerateInstalledAppsWithOptions:completion:` —
  full app scan) -> installd, which is mid-`performScanWithError:` and
  PROGRESSING (thread born, stages advancing). containermanagerd at 32+
  threads (EXC_RESOURCE watermark, survives). installd burns CPU (166
  CPU-monitor denials, no jetsam kill).
- run86's AVKit "spinner" (T416) vanished in run87: transient, not causal.
- CONCLUSION: slow-not-stuck boot storm on 1 CPU (fault storms, eMMC,
  serial spew). The "render wedge/vsync unsubscribe" was idle (nothing
  committed yet), not a wedge.

## run88 (900 s, tracing off): chain drains, scenes build, no commit yet

- Log wrapped (260 KB ring); early shots lost — ring keeps ~60-90 s.
- SpringBoard passed lsd: CTCategory per-app (~111), icon loads, BKSWorkspace
  + scenes created (status bar, wallpaper, switcher, main layout, dimming).
- Still zero FB presents in tail: first CA commit never came by 900 s.
- iTunesCloud offline errors (-1009) are non-fatal (defaults apply).
- ndoagent SIGABRT noted (recurring, seemingly benign).

## run89 (3600 s): kernel VM panic at ~60 min, still no UI

- `vm_map.c:12042 Assertion failed: !old_entry->needs_copy` in
  `vm_map_fork_share`, panicked task = fresh logd (pid 1481) after the
  previous logd (pid 1480) SIG10-crashed and launchd respawned it.
- Root cause class: asymmetric-strategy object + pending needs_copy
  (vm_remap SYMMETRIC->DELAY + vm_protect VM_PROT_COPY; Apple documents
  the same combo in vm_object_shadow's #if 0 note). The old assert treated
  reachable state as impossible.
- Fix (in tree): complete the deferred copy via VME_OBJECT_SHADOW in the
  asymmetric branch (COPY_NONE: drop the flag); clear stale needs_copy in
  the NULL-object branch. Build green; triggers rarely so verified by
  review + survival of later long runs.
- Same 66 apps re-added + 108 categorizations in the last minute with a
  single long-lived SpringBoard (no crash loop): app registration repeats
  late in boot (trigger TBD — installd rebroadcast? icon reload?).
- Snapshots rescheduled deadline-relative (first at D-240 s, every 30 s x8;
  targets SB/BB/logd/installd) to survive the ring. run90 (600 s) validates.

## Safety notes

- `out/ios12-userdata.dmg` (run81's image) is untouched; rollback = flash it.
- `/tmp/xnu-boot-prev.img` + `/tmp/op2-payload-prev.bin` hold the previous boot
  artifacts; previous `build.log` is at `/tmp/build-prev.log`.
- Do NOT enable `wdt=1`: the 35 s HW-watchdog bite always fires mid-boot and
  looks like a hang/reboot loop.

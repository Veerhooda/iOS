# Handoff: SMP Bring-up Session (Nov 07–08, 2026)

Author: Muse Code session (previous agent). Successor: read this file first,
then `logs/probeB21_ramoops.txt` (the SMP proof) and the git-dirty file list below.
Prior history lives in the sibling conversation folders + `_INDEX.md`.

## 1. Session state at handoff

- **Date/time:** ~02:15 IST, Nov 08. User awake and driving the phone.
- **Big result: DUAL-CORE SMP WORKS.** CPU 1 comes online (`arm_cpu_init(): cpu 1
  online`, `OP2CPU(1): came online successfully!`), runs full iOS userland
  alongside cpu0 for 15–30 min runs, clean deadline resets, eMMC 0 failures.
- **Device:** OnePlus 2 (MSM8994, Little cluster cpu0+cpu1 = Cortex-A53) in TWRP
  between runs (`ebd7ee57 recovery`), adb available.
- **IN FLIGHT — B23 run (Home-button fix test):** booted via fastboot at
  **02:11:40**, `cpus=2 op2_deadline=1800 op2_tapms=30000`, image `/tmp/smpB23.img`
  (B23 kernel = B22 + OP2Buttons service). **Verdict due ~02:41–02:45:** wait for
  `adb` recovery, then `adb pull /sys/fs/pstore/console-ramoops-0
  logs/run181_b23_ramoops.txt` and check: `OP2 Buttons:` lines (service start,
  backboardd open/port), `posted key 0x40`, AUTOHOME effect (unlock? surface
  30+? display stays awake?), presents flow, app spawns/deaths.
- **Working tree is DIRTY (do not lose it):** xnu nested repo has 33 modified +
  7 new files (list in §6); main repo has 2 modified + 4 untracked files.
  Nothing committed or pushed this session. Last push: `2743473` (README).
- **out/kernel.development.msm8994 = B23 kernel** (built 02:0x, BUILD_RC=0).

## 2. Device / run cheat sheet

```sh
./tools/build-xnu.sh                                   # build; check BUILD_RC=0
cp xnu/xnu-4903.270.47/BUILD/obj/kernel.development.msm8994 out/
B="-v debug=0x10014e serial=3 cpus=2 keepsyms=1 op2_deadline=900 rd=emmcroot0 emmcroot=userdata rootrw=1 emmcrw=1 cs_enforcement_disable=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 op2fb_client_ui=1 op2_tapms=30000 op2_userlog=0"
python3 tools/mkpayload.py out/kernel.development.msm8994 out/op2.adt /tmp/plX.bin --cmdline "$B"
python3 shim/build.py --payload /tmp/plX.bin --output /tmp/smpX.img   # REQUIRES both flags; defaults boot a STALE image
adb reboot bootloader; sleep 12; fastboot boot /tmp/smpX.img
adb pull /sys/fs/pstore/console-ramoops-0 logs/<name>_ramoops.txt      # from recovery only
```

- **Probe mode:** add `op2_smptest=1` (+ optional `op2_appark=N`, `op2_apmailbox=1`):
  verdict in ~90 s, autonomous panic reset to recovery.
- **Timers (user rule):** ≤15 min until boot animation seen, 30 min max once
  booted. Single-core boots in ~15 min. Never set timers >30 min.
- **Keys are BROKEN:** user can only force-reboot via power long-press (reply
  `rebooted`). No key combos, no volume keys.
- **Force-reboot = COLD reset: wipes pstore DRAM.** Evidence is lost unless the
  device reset itself warm (watchdog/panic/deadline → recovery preserves DRAM).
  Never boot Android/TWRP between XNU runs if you want the ring: Android's
  ramoops driver reformats the region. Chain XNU boots directly from fastboot.
- **Fastboot USB can die** (phone shows fastboot logo, Mac sees nothing):
  ask user to unplug/replug, then try a DIFFERENT Mac USB port. If stuck,
  force-reboot → fastboot visible again.
- **Recovery if `fastboot reboot recovery` fails:** `fastboot boot
  backup/recovery.img` (64 MB TWRP) → adb.
- Ring capacity 260 KB; long runs wrap early-boot content (use deadline dumps).

## 3. SMP bring-up arc (what happened, in order)

- **B9 and before:** PSCI absent (`rc=-1`); SCM `0x02000111` set-boot-addr +
  ACC/LDO unclamp works. `CleanPoC_Dcache()` way/set loop was an infinite spin
  (adds+b.cc never carries in 64-bit) — fixed with region cleans. TEXT is RX:
  runtime TEXT patching faults (B8 lesson).
- **B10 (mailbox):** AP scratch entry (one magic store + sleep, PC-relative, no
  patching) proved **power + entry delivery**: `AP mailbox: 0xe001a9b1`. cpu0
  survives; XNU path dies within ms of SEV.
- **B12/B13 (park-site bisection, one build, `op2_appark=N`):** sites 1–9 all
  PASS individually (reset vector → match loop → start_cpu → common_start →
  MMU-on → trampoline). Site 8 initially died from MY gate bug (double KVA
  conversion post-`br`); fixed → passes. **LR captured = `arm_init_cpu`**
  (stock `ret` target was correct all along).
- **B14 (warm-kick purge + trampoline redirect):** added pre-MMU `tlbi/vmalle1 +
  ic/iallu` (+ a `dc isw` loop, see B22 lesson) and replaced trampoline `ret`
  with `bl arm_init_cpu` (x0 = cpu_data VA from phystokv).
- **B15–B19 (C-crumbs + AP console tags):** `OP2_AP_CRUMB` + `op2_ramoops_ap_write`
  (lock-free console lines, survive dual-core death). AP reached `APC1 → APG4`
  then died. **Root cause (external analysis, VERIFIED): `cpu_number()` NULL
  deref** — `op2_secondary_cpu_init()` ran before `machine_set_current_thread()`
  (TPIDR=0), and `op2_vtimer_set_decrementer`'s deadline check calls
  `cpu_number()` → data abort → double fault (sleh also uses getCpuDatap).
- **B20 (SMPEN attempt): FAILED.** `mrs/msr S3_1_C15_C2_1` (CPUECTLR) and CBAR
  reads **trap from NS-EL1 to EL3** on this TrustZone (needs ACTLR_EL3 grant QC
  doesn't give). Froze cold boot at the 1+ logo with ZERO output (common_start
  runs on cpu0 too). Lesson: TZ owns coherency/SMPEN (sets it during SCM
  bringup); never touch CPUECTLR/CBAR from EL1. B20's run also taught: stuck
  fastboot + dead USB → force-reboot (cold, wipes pstore).
- **B21 (the fix): CPU 1 ONLINE.** Removed all CPUECTLR/CBAR; x21-guarded the
  pre-MMU block; moved `machine_set_current_thread` before
  `op2_secondary_cpu_init`; TPIDR-guarded `cpu_number()` in set_decrementer;
  literal `1` in the secondary kprintf. All 17 AP tags (APC1–APCC, APG1–APG5),
  `arm_cpu_init(): cpu 1 online`, clean probe reset. **Proof: `logs/probeB21_ramoops.txt`.**
- **B22 (dinv removal):** external analysis caught TWO bugs in my `dc isw` loop:
  `#2` selects **L2, not L1** (level field is Level−1), and 32-bit `clz` put the
  way at bits 31:30 instead of 63:62 → it discarded arbitrary **shared-L2**
  lines holding cpu0's live data. Removed the loop entirely (TZ resets AP L1
  clean); kept `tlbi + ic iallu`. Also moved site-5 park inside the x21 guard.
- **run178 (first full SMP, 15 min, NO autotap):** cpu0+cpu1 online whole run,
  1.5M eMMC reads 0 fails, clean deadline reset. Display: presents climbed to
  165 then froze at min8, panel dark. **Diagnosis (mine + external, VERIFIED in
  log): NOT an SMP bug — idle display sleep.** Missing `op2_tapms` → single-pass
  taps ended → lockscreen idle → backboardd `RequestPowerChange(0)` → `display
  off`, surface 3. Physical taps don't unlock (need Home + swipe).
- **run179/C21 (single-core control, 30 min, idle, B21 kernel):** lockscreen +
  home + Settings all USER-DRIVEN OK. Presents froze at min16 the same way
  (idle sleep after user stopped) → confirms SMP-independence. Captured a
  Settings SIGABRT (shared abort path, see §5).
- **run180 (SMP + autotap, 30 min, B22):** dual-core stable full run, display
  slept/woke normally (surface 18 lit min25–26, user in Clock app via Control
  Center), 20 AUTOHOME/AUTOSWIPE + 44 physical taps. **User report: AT dot
  visible but AT-Home does nothing; stuck in apps.**
- **B23 (OP2Buttons, IN FLIGHT):** separate Consumer/ConsumerControl HID service
  for Home (see §4). Rationale below.

## 4. AT-Home / AUTOHOME root cause + OP2Buttons fix

- **Evidence:** synthetic Home has NEVER had UI effect in any run (46+
  AUTOHOMEs loop-28, run180 AUTOHOMEs, AT-Home). Sender side verified
  well-formed: proper Consumer/Menu (0x40) keyboard packet, 150 ms press,
  working queue. Digitizer declares ONLY Digitizer(13)/TouchScreen(4) usages +
  one element.
- **Research:** WebDriverAgent posts Home (0x0C/0x40) through backboardd on
  stock iOS (works) → drop is in OUR delivery. Sister project **ios-sweet**
  (Redmi Note 10 Pro iOS 16 port, cloned to `/tmp/ios-sweet`) uses a SEPARATE
  `SweetHIDButtons` service (PrimaryUsagePage=Consumer, Usage=ConsumerControl)
  for power/volume/home + separate touch service. backboardd routes
  keyboard-typed records only from keyboard-typed services
  (`dispatchKeyboardEvent` vs `dispatchDigitizerEvent`).
- **Fix (B23, in tree, built clean):** new `OP2Buttons : IOHIDEventService` +
  `OP2ButtonsUserClient` in `OP2Digitizer.cpp` (same TU, no build changes) +
  catalog entry in `KernelConfigTables.cpp`. Consumer/ConsumerControl props,
  Button element (page 12, usage 0x40), own queue/lock/senderID,
  `key(usage,down)`. `postHomeButtonEvent` routes to `gOP2Buttons` when
  present, else legacy digitizer-queue post (fallback preserved).
- **AT note:** assistivetouchd was jetsam-killed at min5 in run180 (36 MB
  inactive-hard limit) but the dot was visible later → it relaunches; still,
  1 GB-device jetsam profile on 4 GB hardware kills daemons aggressively.
- **To verify on B23 verdict:** `OP2 Buttons:` start/backboardd-open/port lines,
  `posted key 0x40`, and UI effect (unlock/wake/home).

## 5. Crash/app findings (pending capture work)

- C21 captured **Settings SIGABRT** (Preferences pid 173) + Spotlight SIGABRTs,
  all via shared abort path: pc `libsystem_kernel+0x230dc` (__pthread_kill),
  lr `libsystem_pthread+0x2094` (slide `0x8c220000`). The WHY needs deeper
  stacks (PROC_EXIT_CRASH has pc/lr only).
- **CrashReporter avenue is DRY:** on-device `/var/mobile/Library/Logs/
  CrashReporter` has only stale OTAUpdate .ips (1969 dates — no RTC) +
  com.apple.appstored dir; DiagnosticLogs EMPTY; Assistant only Analytics.
  ReportCrash doesn't write app logs on this port. Use exit-ring PCs + SNAP
  stacks + (later) deeper in-kernel stacks instead.
- **NOT yet captured:** Photos crash, Wallpaper/Settings panes (user must open
  Photos + Settings→Wallpaper with timestamps on a UI run).
- Dyld cache restored for symbolication: mount `out/ios12-userdata-fixA.dmg`
  readonly at `/tmp/fixaro` (default `OP2_DYLD_CACHE` path). `out/cache-hfs.dmg`
  and `out/system-hfs.dmg` do NOT contain it (only dev/sbin).
  `op2sym.py <cache> <slide> <pc>... [--exec <base>]`.

## 6. Exact tree changes (all UNCOMMITTED)

xnu nested repo (`xnu/xnu-4903.270.47`, 33 modified + 7 new, +1868/−116):
SMP/Sw-relevant: `osfmk/arm64/start.s` (mailbox scratch, parks 1–9, warm-kick
purge, trampoline `bl arm_init_cpu`), `osfmk/arm/arm_init.c` (C-crumbs C1–CC,
thread-attach reorder), `iokit/Kernel/arm/OP2CPU.cpp` (mailbox/crumbs/parksel,
probe panic, NO cpuectlr), `pexpert/arm/pe_identify_machine.c` (G-tags G1–G5,
TPIDR guard, `Secondary CPU 1` literal), `pexpert/arm/pe_serial.c`
(`op2_ramoops_scratch`, `op2_ramoops_ap_write`), `pexpert/.../board_config.h`
(CPU_COUNT=2), `iokit/Kernel/arm/OP2Digitizer.cpp` (OP2Buttons service, NEW
file), `iokit/KernelConfigTables.cpp` (OP2Buttons entry). Plus pre-existing
port files (eMMC, FB, Surface, I2C, snapshot, sleh, exit ring, ADT tools).
Main repo: `M research/hunkCV-sb-crash.md`, `M tools/mkadt.py`, `??
History/ tools/hunkMA.py tools/hunkMA2.py tools/op2hfs.py`, stray `?? --help`
(delete it). Reconstruct via `git status/diff` in both repos.

## 7. Pending tasks (in order)

1. **B23 verdict** (~02:41–02:45): pull `logs/run181_b23_ramoops.txt`, check §1
   items. If Home works → tell user to test AT-Home + AUTOHOME unlock.
2. **Photos/Settings capture:** user opens Photos till crash + Settings→
   Wallpaper/panes with timestamps; correlate with exit ring + slide.
3. **AT/Home verification** on winning config; jetsam pressure is a suspect
   for daemon flakiness (1 GB profile).
4. **Stability soak** (8 h was discussed; user caps individual runs at 30 min —
   chain runs instead), trim extension, physical buttons.
5. Optional/deferred: `op2fb_keepon` boot-arg (ignore RequestPowerChange(0)
   while testing); user declined for now.
6. Commit + push when user asks (never unasked). xnu nested repo was never
   pushed (only docs reference it).

## 8. User constraints (standing)

Thorough external research before fixes (jailbreak/iOS docs, sister projects:
hakios-xnu-kernel, ios-sweet); minimal verified changes; preserve touch
framing; timers ≤15 min pre-animation / 30 min max; broken keys (force-reboot
only, reply `rebooted`); don't strand the phone (must stay visible in
fastboot/adb); user drives UI when awake (note crash times); short runs
preferred; no commits/pushes unasked; user-facing README (no internals).

## 9. External analyses received (all kept, all used)

1. hunkMA2/run177/SMP-audit report → verified PSCI-first, SCM reg bug (fixed),
   LDO/L2 notes. 2. SMP crumb-plan audit → verified claims before applying.
3. B19 NULL-deref + B20 CPUECTLR-trap report → both VERIFIED and fixed (B21).
4. `dc isw` level/shift bug report → VERIFIED (L2 + garbled way) and fixed
   (B22 removal). 5. run178 display-sleep report → VERIFIED in log; its
   op2_tapms recommendation was already applied (run180); keepon deferred.
Note: analysis #5's hardware table says A57 — wrong cluster, we're on A53
Little (cpu0–1); the rest held up.

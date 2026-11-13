# OnePlus 2 iOS 12: implementation and guidance plan (written 2026-11-12, end of session)

Audience: the next model or engineer taking this over cold. Read this file first, then CLAUDE.md (safety rules), then
docs/FOUNDATION-BENCH.md (why it is slow), docs/FABLE-REVIEW.md sections H..K (the GPU state and today's findings),
docs/GPU-PLAN.md only if you work on the GPU.

## 0. Where things stand

iOS 12.5.7 (iPhone 6 Plus userland, N56) runs on a OnePlus 2 (Snapdragon 810) under our XNU 4903 build, RAM-booted
from TWRP with `fastboot boot`. With `metal=3`, CoreAnimation for every process composites on the Adreno 430 through
our Metal shim (userland/OP2Metal) over Mesa freedreno (userland/op2mtl). The lock screen, home screen (wallpaper,
icons, dock blur), page swipes, Settings, Photos, etc. work. The user tests by hand; Home is the fingerprint sensor
or a bottom strip of the touch screen (the hardware Home emulation in OP2I2C).

What the user experiences today: works, but sluggish; Home slow to respond for a while after boot; apps slow to
open; the phone gets **very hot**. Turning on iOS Low Power Mode by hand made it noticeably cooler and snappier
(less background work). That is the key clue for the next steps.

Measured (docs/FOUNDATION-BENCH.md): CPU, memory, timers, IPIs are healthy for the four Cortex-A53 cores at 1.5 GHz.
Storage was the bottleneck (polled PIO, 16.7 MB/s); ADMA2 DMA reads (this session) doubled page-in speed
(640 -> 285 us per 16 KB, 42 MB/s sequential). Background daemons crash-loop constantly and burn CPU + storage.
There is NO thermal management in our kernel at all.

## 1. Hard rules (from CLAUDE.md + learned today)

- Volume/Power buttons are broken. The only way out of a running iOS is the software deadline (`op2_deadline`,
  set by DEADLINE= in tools/round.sh). Every boot image must carry it.
- Never `fastboot flash`. Only `fastboot boot out/xnu-boot.img` (tools/boot-daily.sh does it; BCB cookie keeps every
  reset landing in TWRP).
- Don't raise CPU voltage beyond op2_cpu_boost's stock values. Never use SMMU ATS (resets the SoC).
- `xnu/` is gitignored: back up kernel files you edit to kernel-backup/round6-gpu/<tag>/ before and after.
- **New: heat.** Give the phone >= 15 min off between long rounds until thermal throttling exists (task 1). Ask the
  user to unplug USB if it is hot. Never run back-to-back 30-minute rounds for hours again.
- Commit only when the user asks; work is on branch `gpu-fable-review` (see section 6 for what is uncommitted).

## 2. How to work (tools and gotchas)

- Round: `EXTRA="metal=3" DEADLINE=1800 bash tools/round.sh` (build kernel + boot + wait for reset + pull log).
  Run it in the background and wait for completion; the log is logs/daily-<time>.log, summary logs/last-summary.txt.
  `AUTOIN=3` gives the unattended gesture script (op2_autoinput=3), default 1 = wake pulses only.
- Kernel cmdline must stay < 250 bytes (tools/build-daily.sh refuses). Short boot-arg names.
- The log is ramoops, ~260 KB: early lines get overwritten in long rounds. Keep messages terse; the most important
  facts should be printed late too (e.g. in op2_emmc_dump_stats at deadline).
- Phone files are edited from TWRP with tools/op2hfs.py (`ls`, `cat`, `read`, `write`), in place only: a file can
  not grow or be created. Install binaries into the padded slots (op2mtl.dylib 24 MB, OP2Metal 1 MB). Always
  write from a frozen copy (out/*.installed) and read back + cmp. op2hfs now follows the catalog's extents (the
  catalog grew a second extent; the old code read garbage).
- A new file or a bigger file needs a rootfs rebuild + flash of userdata: tools/rootfs-journal.sh from
  out/ios12-userdata-fixA.dmg. That loses on-phone edits not also applied to the DMG (check tools/rootfs_g5.py,
  tools/rootfs_prefs.py for what gets applied). Prefer in-place edits.
- Running a userland program at boot: tools/deploy-probe.sh <binary> (replaces /usr/libexec/tzinit, a launchd boot
  task; `--restore` puts the original back -- do it after measuring). userland/op2bench is the foundation benchmark;
  it logs `BENCH` lines ~150 s and ~5 min after boot.
- Crash diagnosis:
  - Kernel prints `PROC_EXIT_CRASH: pid=..[name] pc lr sp x0 x1 x16` for every crashing process.
  - In processes OP2Metal is loaded into (metal=3: those that use Metal/CA), OP2Metal logs uncaught exceptions and
    fatal signals as `OP2 GPU user: C <proc>: ...` (reason, __crash_info messages, image+offset frames).
  - Shared-cache slide: a crash in `__pthread_kill` has pc = slide + 0x180a630dc + 8 (libsystem_kernel at
    0x180a40000 in our cache). Unslide, then `tools/dyld_extract.py <cache> attr <va>` names the image+offset.
  - `tools/dsc_dis.py <cache> <start> <end>` disassembles with selector/string names; `tools/dsc_exports.py` +
    `tools/dsc_export_off.py` list an image's exports. The cache: mount out/ios12-userdata-fixA.dmg read-only
    (`hdiutil attach -readonly -nobrowse -mountpoint <dir> ...`), path System/Library/Caches/com.apple.dyld/.
  - Kernel panics: `atos -arch arm64 -o xnu/.../kernel.development.msm8994.unstripped <lr...>`.
- Frame captures: backboardd (metal>=2) writes a half-size frame every 40 s to /private/var/tmp/op2cap-<n>.bgra;
  `bash tools/op2cap-pull.sh` in TWRP converts them to logs/cap/*.png. Use them when the user is away.
- Shader cache (op2mtl): /private/var/mobile/Library/Caches/op2mtl, warm after one boot. OP2MTL_NOCACHE disables.

## 3. Tasks, in priority order

### Task 1: thermal safety (do this first, before any long round)

Goal: never let the SoC cook; log temperature on every `OP2 TOP` line; throttle; hard cutoff.

- Sensor: TSENS at PA 0xfc4a8000 (size 0x2000), 16 sensors, calibration in eeprom 0xfc4bc000 region
  (reference/kernel-oneplus-msm8994/arch/arm/boot/dts/qcom/msm8994.dtsi ~line 3199, driver
  drivers/thermal/msm8974-tsens.c, "qcom,msm8994-tsens"; slopes in the dtsi). Read the per-sensor status registers
  (TSENS_S0_STATUS + 4*n in the TM region; see the driver for the 8994 offsets and the code-to-degrees conversion
  with the fuse calibration). Start with raw codes logged per sensor, then convert. If LK did not enable TSENS,
  enable per the driver's init. Reading an unexpected secure region can reset the SoC: that only lands in TWRP,
  but go slowly (one register, one boot).
- Which sensors: the dtsi's msm_thermal node (~line 3561) and the thermal-engine config in a stock OnePlus 2
  firmware name the CPU cluster and GPU sensors.
- Add to osfmk/arm64/op2_snapshot.c's `OP2 TOP` line: max CPU temp, GPU temp.
- Throttle: above ~75 C drop op2_cpu_boost back to the lower stock frequency (OP2Framebuffer.cpp op2_cpu_boost /
  op2_cpu_mhz_now) and drop the GPU RPM vote (OP2GPU.cpp, "RPM gfx3d_clk_src vote") from 300 MHz to a lower step;
  restore below ~65 C. Hard cutoff: above ~90 C, sync and panic-reset to TWRP (same path as the deadline,
  osfmk "software timer deadline exceeded"), whose Android kernel has thermal control.
- Verify with a 15-min round: temperature trace in the log, user reports less heat.

### Task 1b: USB console, shell, ECM, SSH (done 2026-11-13; docs/USB-CONSOLE-PLAN.md stages 1-3)

Live kernel log over CDC-ACM (stage 1), root dash shell on /dev/op2usb with job control + toybox
(stage 2; isatty needed d_type D_TTY since iOS 12 checks FIODTYPE), composite CDC-ECM with ue0
192.168.7.2/24 + ping, and dropbear SSH (exec + scp -O + pubkey auth verified; host keys via -R) all work.
Launcher: userland/op2shell (embeds dash/toybox/dropbear/scp/authorized_keys), installed over the
op2-air-run slot. Remaining: interactive-pty ssh session by hand, OrangeTree blank-screen game debugging
(out/games, tools/games/build-orangetree.sh) with the shell.

### Task 2: quiet background by default (the Low Power Mode effect, permanently)

Evidence: the user enabled Low Power Mode by hand and the phone got cooler and snappier. Background work competes
with the UI on four in-order cores and a slow eMMC.

- Find where iOS 12 stores Low Power Mode: have the user toggle it, then read candidate plists with op2hfs before/
  after and diff (likely under /private/var/mobile/Library/Preferences/ or /private/var/db/ -- powerd /
  com.apple.coreduetd*). Then set it in place (same-size edit, like the MobileGestalt plist edit: re-serialize
  with plistlib and pad before the offset table; see the script used in FABLE-REVIEW K / backup/rootfs/*.g5c-*).
- Also off by default: Background App Refresh, Spotlight indexing (the Spotlight importers crash-loop anyway),
  automatic downloads, iCloud/Siri suggestions. Prefer preference plists edited in place; a launchd disable list
  (/private/var/db/com.apple.xpc.launchd/disabled.plist) needs a rootfs rebuild if it must grow.
- Measure: `OP2 TOP` busy % at idle before/after, the user's feel, temperature (task 1).

### Task 3: stop the crash loops

Per round (30 min): dataaccessd ~67 crashes, logd ~6 (SIGBUS/SIGSEGV loop: stack probe), five Spotlight importers
~6 each (SIGABRT), siriactionsd, PhotosFileProvider, LocalStorageFileProvider, itunescloudd. Each relaunch reads
the binary + libraries from eMMC and costs launchd CPU. Photos (MobileSlideShow) aborts on launch; Wallet was
killed by the launch watchdog (0x8badf00d, main thread in mach_msg = waiting on a daemon).

- For each: symbolize the crash (section 2 recipe). OP2Metal is not loaded into most daemons, so their exception
  text is not captured: extend the crash capture -- e.g. a tiny dylib injected via a DYLD_INSERT_LIBRARIES-free
  route is hard; easier: the kernel already prints pc/lr; dataaccessd and the Spotlight importers all stop at
  __pthread_kill (abort), so find the abort reason by symbolizing lr and the caller chain (read the stack: add
  a few frames from the user stack via the frame pointer in the kernel's PROC_EXIT_CRASH printer).
- Then either fix the cause (a missing service, sandbox path, MobileGestalt key, IOKit class) or disable the daemon
  (we have no accounts, iCloud, cellular, Siri). Disabling via launchd plist edits must keep file sizes, or go
  through a rootfs rebuild.
- logd's crash is old (CLAUDE.md "logd SIGBUS/SIGSEGV loop (stack probe)"): a stack-probe fault on a guard page;
  check the thread stack size / guard handling in our kernel for logd's thread.

### Task 4: eMMC faster bus (HS200 or DDR52)

ADMA2 is in (bsd/dev/op2_emmc.c `sdhc_read_dma`, self-tested, 0 failures). The bus is still high-speed SDR 52 MHz
(42 MB/s). The next boot prints `OP2 eMMC: bus: clock ctl .. host ctl .. host ctl2 ..` -- confirm the mode first.
DDR52 (EXT_CSD HS_TIMING=1 + BUS_WIDTH=6, host DDR50 in HOST_CONTROL2) is the smaller step; HS200 needs 1.8 V I/O,
clock 200 MHz from GCC, and tuning (CMD21) with the MSM DLL (reference drivers/mmc/host/sdhci-msm.c). Both carry
real risk of a wedged card: keep the PIO/DMA self-test pattern, fall back to the current mode on any failure, and
never touch writes until reads are proven over several boots. Also: DMA for writes later; interrupt-driven
completion instead of polling (lets the waiting thread sleep).

### Task 5: the big cores (Cortex-A57 x4)

Only the A53 cluster runs (cpus=4, OP2CPU.cpp). The A57s are roughly 2x per thread. Bring-up needs the APC1 rail,
the A57 L2 power-up and the cluster clock (reference/kernel-oneplus-msm8994 arch/arm/mach-msm/ cpu power-up for
"qcom,8994-arm-cortex-acc"/l2 and drivers/clk/qcom/clock-cpu-8994.c). Respect the voltage rule (stock values only).
Thermal (task 1) must exist first: the A57 cluster is what made the 810 notorious.

### Task 6: GPU-side performance (after the foundation)

docs/FABLE-REVIEW.md and docs/PEER-FINDINGS.md: async commits (frames are committed synchronously today), GPU
scan-out instead of the CPU downsample (1242x2208 -> 1080x1920, ~6 ms/frame) e.g. via the MDP VIG scaler,
op2mtl texture pool / BO reuse. Peers measured 20 -> 38 fps from async commits + GPU scan-out.

### Task 7: iPhone X gestures (parked)

Tried and reverted (FABLE-REVIEW K). The blocker: bottom-edge swipes never trigger iOS system edge gestures (also the
6 Plus Control Center swipe-up), while top-edge swipes do. Find how backboardd decides a touch begins at an edge
(the digitizer HID event fields our OP2Digitizer posts vs a real device's: edge mask / "touch began outside the
display" flags; backboardd BKSystemGesture / BKHIDTouch). Fixing that fixes the 6 Plus Control Center swipe too.
Then: MobileGestalt HomeButtonType=2 (backup/rootfs/com.apple.MobileGestalt.plist.g5c-homegesture) + the CC NaN
crash (CCUIStatusBar setOffset:forPartWithIdentifier: with CGRectNull frames; guard in OP2Metal).

## 4. Fixed this session (for orientation)

- op2mtl translation cache: warm boot translates 0 shaders (4.5 s load vs ~99 s). Cache dir moved to
  /private/var/mobile/Library/Caches/op2mtl (/var/tmp did not keep it).
- Settings > Wallpaper crash: kernel IOSurface values (OP2Surface sel 9/10, state page +0x18/+0x20).
- Kernel panic "IOGMD: not wired for the IODMACommand": >4 KB struct in/out descriptors now prepare()d.
- Black dock: newTextureLayoutWithDescriptor answer disabled (OP2METAL_TEXLAYOUT=1 to re-enable).
- OP2Metal crash reports; NSSet guard for the X Control Center path.
- eMMC ADMA2 reads.
- op2hfs catalog extents.

## 5. Verification habits that worked

- One change per boot when it can break the boot; several when they are independent and each is gated (self-tests,
  boot-args to disable).
- A self-test that compares the new path with the old byte for byte, logged PASSED/FAILED, auto-fallback.
- Benchmarks over impressions: op2bench before/after.
- Read the user's photos/videos carefully but trust measurements; frame captures when the user is away.

## 6. Uncommitted work (branch gpu-fable-review, base 1a00418)

Modified: docs/FABLE-REVIEW.md, tools/build-daily.sh (AUTOIN, op2fb_hz dropped), tools/op2hfs.py (catalog extents),
userland/OP2Metal/OP2Metal.m (crash reports, NSSet guard, texture layout off, layout class), userland/op2mtl/op2mtl.c
(translation cache). New: docs/FOUNDATION-BENCH.md, docs/NEXT-PLAN.md, userland/op2bench/, tools/dsc_*.py,
kernel-backup/round6-gpu/fable-30 .. fable-43 and fable-final (the latest copy of every kernel file changed this
session: OP2GPU, OP2Surface, OP2I2C, OP2Digitizer, op2_emmc.c). On the phone: op2mtl fable-7, OP2Metal fable-14,
original MobileGestalt plist and BaseBoard bytes (gestures reverted), original tzinit.

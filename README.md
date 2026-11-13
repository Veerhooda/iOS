# iOS on OnePlus 2

Apple **iOS 12.5.7** (`16H81`, built for the iPhone 6 Plus, `n56ap`) running on a **OnePlus 2** (`ONE A2003`,
Qualcomm Snapdragon 810 / MSM8994) through a custom port of Apple's open-source XNU kernel. The phone boots from
Qualcomm's own bootloader into a small shim, XNU takes over, mounts a real HFS+ root on the eMMC, and runs
launchd, backboardd and SpringBoard to the Lock Screen and Home Screen.

The kernel lives in its own repository: [Veerhooda/xnu](https://github.com/Veerhooda/xnu) (branch `msm8994`).


---

## Status (2026-11-11)

| Area | State | Evidence |
|---|---|---|
| Boot | LK `boot.img` → shim → XNU → launchd → SpringBoard, Lock Screen and Home Screen | every round in `logs/` |
| CPU | 4 × Cortex-A53, clocked to **1536 MHz** (`op2_cpu_boost`, stock voltages) | `OP2 TOP:` lines, measured with the PMU cycle counter |
| Storage | SDHCI eMMC 5.0, multi-block reads (CMD18) with a boot self-test, journaled HFS+ root | `OP2 eMMC MULTI: … fails=0` |
| Display | MDP5 command-mode panel, double-buffered, SwapWait honoured, ~6 ms CPU downsample 1242×2208 → 1080×1920 | `OP2 FB PERF:` lines |
| Input | Synaptics S3320 touch; Home = a touch starting on the bottom strip; synthetic input only to wake (`op2_autoinput=1`) | |
| **GPU (Adreno 430)** | **Draws correctly on the hardware** through Mesa's freedreno driver, including CoreAnimation's own shaders; not yet used by the UI | [docs/GPU-PLAN.md](docs/GPU-PLAN.md) |
| Known crashes | logd SIGBUS/SIGSEGV loop, Spotlight extensions SIGABRT loop, occasional SpringBoard SIGSEGV | [CLAUDE.md](CLAUDE.md) |

### GPU bring-up, stage by stage

Everything here was measured on the phone; the details and logs are in [docs/GPU-PLAN.md](docs/GPU-PLAN.md).

| Stage | Result |
|---|---|
| **G0** kernel driver | Command processor really executes (it never had: it was "halted" with the wrong bits), GPU SMMU set up through TrustZone (SMC `restore_sec_cfg` after a secure page-table pool), core clock voted through the RPM, memory-write fences, hang detection and recovery. 1000/1000 checked submissions, 1000-submission burst through a ring wrap. |
| **G1** command-processor blits | Userland library [`userland/op2gpu`](userland/op2gpu) (fill and copy byte-exact on the phone, same hash as the host's fake GPU). |
| **G2** shader compiler | Mesa 25.0.7 + hakios's Metal (AIR) front end built on the Mac, patched for iOS 12's AIR. **Every CoreAnimation pipeline the phone actually asked for compiles for the A430**; 61 of 62 QuartzCore functions with all constants zero. |
| **G3** first draw | Mesa's own fd4 driver, cross-built for iOS 12, draws on the phone through our kernel: pixel exact. |
| **G4** what CoreAnimation needs | Texture sampling, varyings, constant buffers, premultiplied blending, BGRA8 targets at full panel size (2 073 600 of 2 073 600 pixels exact), CoreAnimation's real uber shader with its captured function constants. |
| **G4c** open problem | The uber shader drawn at full panel size **writes outside its buffers**, and with the SMMU in bypass that corrupts kernel memory. The fix in progress is real SMMU translation (identity map of live buffers only), so such a write faults instead. |

---

## How a test round works

The phone has no shell and broken volume/power buttons. Every boot is a RAM boot of a freshly built image, with a
software deadline that resets the phone back into TWRP, where the kernel log is pulled from ramoops.

```bash
bash tools/round.sh                 # build kernel + boot image, RAM-boot, wait for the reset, save the log
DEADLINE=300 bash tools/round.sh    # shorter run
cat logs/last-summary.txt           # summary; full logs in logs/daily-*.log
```

* `tools/build-daily.sh` builds the kernel and `out/xnu-boot.img` (boot-args in one place, `EXTRA=` appends).
* `tools/boot-daily.sh` re-arms the recovery cookie and `fastboot boot`s the image. It never flashes.
* `tools/pull-log.sh` pulls `/sys/fs/pstore/console-ramoops-0` and writes the summary.
* `tools/rootfs-journal.sh` rebuilds the journaled rootfs from the clean image and streams it to userdata
  (needs `sudo` on the Mac for file ownership inside the image).

### Safety rules (the phone is hard to recover)

* Never `fastboot flash`. Boot only with `fastboot boot out/xnu-boot.img`.
* Keep the recovery (BCB) cookie armed so every reset lands in TWRP; `tools/boot-daily.sh` does it.
* Every boot image carries `op2_deadline` — the hardware watchdog strands the phone, so the software deadline is
  the only way out of a hang.
* Do not raise CPU voltages beyond the stock values used by `op2_cpu_boost`.

---

## Building the GPU stack

```bash
bash tools/mesa-host.sh             # Mesa + AIR front end for this Mac (out/mesa-host): air2nir, ir3_compiler
bash tools/ca-sweep.sh              # every QuartzCore function through AIR -> NIR -> ir3 for the A430
bash tools/ca-compile-captured.sh   # CoreAnimation's real pipelines, with the constants captured on the phone
bash tools/mesa-ios.sh              # Mesa + freedreno cross-built for iOS 12 (out/mesa-ios): air_run
make -C userland/op2gpu test        # the CP blit library against its fake GPU
```

Getting a program onto the phone and started:

* `tools/op2hfs.py` reads and rewrites files on the HFS+ userdata partition in place over adb (TWRP has no hfsplus).
  It cannot grow a file.
* iOS 12's launchd reads its jobs only from `/System/Library/Caches/com.apple.xpcd/xpcd_cache.dylib`.
  `tools/xpcd-cache-add.py` adds a job to that cache without changing the file's size (boot-arg
  `launchd_unsecure_cache=1` lets launchd accept it). A plist dropped into `LaunchDaemons/` alone is ignored.
* `tools/fc-from-log.py` turns OP2Metal's function-constant capture lines (boot with `metal=2`) into compiler input.

Host requirements: macOS, Xcode 10.3 (its iPhoneOS 12.4 SDK and libc++ headers), Python 3, `adb`/`fastboot`.
Mesa's build tools (meson, ninja, bison 3.8) are installed by the scripts into `out/`, not system-wide.

---

## Repository layout

* [`shim/`](shim/) — bare-metal boot shim between Qualcomm LK and XNU.
* [`tools/`](tools/) — build, boot, log, rootfs, Mesa and GPU test tooling (see above).
  * [`tools/mesa-op2/`](tools/mesa-op2/) — our patches on top of Mesa and the AIR front end (iOS 12 AIR, `air_run` options).
  * [`tools/ios-iokit/`](tools/ios-iokit/) — the few IOKit user-client declarations the iOS SDK lacks headers for.
* [`userland/OP2Metal/`](userland/OP2Metal/) — the Metal plug-in shell iOS loads for our GPU; also captures function constants.
* [`userland/op2gpu/`](userland/op2gpu/) — command-processor blit library, fake GPU, phone probe.
* [`docs/`](docs/) — [GPU plan and results](docs/GPU-PLAN.md), [audit](docs/AUDIT-2026-11-11.md),
  [how it boots](docs/HOW-IT-BOOTS.md), [kernel changes](docs/OP2_KERNEL_CHANGES.md), [hacks](docs/HACKS.md), history.
* [`CLAUDE.md`](CLAUDE.md) — short working notes: current state, loop, rules.
* `kernel-backup/` — copies of kernel sources before risky edits.
* Not in the repository: `xnu/` (its own repo), `reference/`, `research/`, `logs/`, `backup/`, `out/` — device dumps,
  Apple binaries and build output.

---

## Credits and licences

* **Apple** — XNU (`xnu-4903.270.47`), under the Apple Public Source License 2.0.
* **[oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel)** and **hakios-metal** (MIT) —
  the AIR (Metal) front end for Mesa, the libdrm stand-in, `air_run`, and the GPU layer `userland/op2gpu` is
  adapted from. Their licence is kept as `userland/op2gpu/LICENSE-hakios`.
* **[Mesa](https://mesa3d.org)** (MIT) — freedreno, ir3, NIR. Fetched and patched at build time, not vendored.
* **Qualcomm's KGSL** (GPL, in the OnePlus kernel source) — read as the reference for the A430 bring-up sequence.
* **[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)** — the method this port started from.

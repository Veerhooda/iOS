# iOS on OnePlus 2 (Snapdragon 810 / MSM8994)

Boots **real Apple iOS 12.5.7 userland on a OnePlus 2** (`ONE A2003`, Qualcomm
Snapdragon 810 / MSM8994): XNU 4903.270.47 with a custom MSM8994 board port,
real HFS+ root from eMMC, launchd, SpringBoard, backboardd, touch, and a
working display pipeline. Method inspired by
[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) (Redmi Note
10 Pro), re-engineered for MSM8994 (GICv2, 4K pages, eMMC, LK bootloader).

**Status (2026-11-06): iOS runs.** Latest 2h validation (loop-25): 36,250
display presents, 10 compositor surfaces, lit panel, SpringBoard + backboardd
alive from boot, 2.59M eMMC reads with 0 failures, no crashing daemons. See
[docs/STATUS.md](docs/STATUS.md) for the full evidence log.

## Repository layout

| Path | Tracked | Contents |
|---|---|---|
| `tools/` | yes | Build/pack/flash/analysis scripts (see catalog below) |
| `shim/` | yes | Bare-metal ARM64 LK shim (boot.img kernel) |
| `docs/` | yes | Milestones, boot flow, status, kernel-change index |
| `research/` | notes only (`*.md`, `hardware/`) | Design notes, postmortems, plans |
| `README.md`, `NOTES.md` | yes | This file; Day-0..M6 engineering log |
| `xnu/` | **no** (nested repo, Apple source) | XNU tree, branch `msm8994` (see below) |
| `out/` | no | Build outputs, boot images, userdata DMGs |
| `backup/`, `dtb/`, `recovery/`, `reference/` | no | Dumps, DTBs, stock images, reference trees |
| `logs/` | samples only | Per-run ramoops pulls (`runNNN_ramoops.txt`) |
| `History/` | no | Prior agent session transcripts (local context) |

`xnu/xnu-4903.270.47` is a nested git checkout (base `494ffe1` = pristine
xnu-4903.270.47, branch `msm8994`) whose remote is Apple's
`apple-oss-distributions/xnu`. It is **not pushed here** (Apple source, ~400M
with build dirs). [docs/OP2_KERNEL_CHANGES.md](docs/OP2_KERNEL_CHANGES.md)
indexes every OP2 change so the tree can be reconstructed/audited.

## Prerequisites

- macOS host (Apple Silicon tested), Xcode 10.3 toolchain for the kernel,
  Homebrew LLVM (`llvm-objcopy`, `ld.lld`), `clang`.
- `adb` / `fastboot` in PATH; device with TWRP recovery.
- OnePlus 2 with partitions backed up (`backup/` is git-ignored; keep your
  own copy — persist/modemst/fsg hold IMEI + calibration).
- iOS 12.5.7 (16H81) iPhone 6 Plus (`iPhone7,1`) root filesystem prepared as
  `out/ios12-userdata-fixA.dmg` (6 GB HFS+; build/flash flow below).

## Build / flash / boot loop

```bash
# 1. Build the kernel (incremental; ~2-10 min)
./tools/build-xnu.sh
cp xnu/xnu-4903.270.47/BUILD/obj/kernel.development.msm8994 \
   out/kernel.development.msm8994

# 2. Device tree + payload + boot image (NOTE: both flags required)
python3 tools/mkadt.py out/op2.adt
python3 tools/mkpayload.py out/kernel.development.msm8994 out/op2.adt \
    out/op2-hardware-payload.bin --cmdline "<see below>"
python3 shim/build.py --payload out/op2-hardware-payload.bin \
    --output out/xnu-boot.img

# 3. Flash userdata (device in TWRP/recovery with adb)
python3 tools/flash_userdata.py out/ios12-userdata-fixA.dmg

# 4. Boot (proves nothing until the log is pulled)
adb reboot bootloader; sleep 12
fastboot boot out/xnu-boot.img   # auto-resets to recovery at op2_deadline

# 5. Pull the verdict
adb pull /sys/fs/pstore/console-ramoops-0 logs/runNNN_ramoops.txt
```

Reference cmdline (≤250 chars, verified in `out/xnu-boot.img` bytes):

```text
-v debug=0x10014e serial=3 cpus=1 keepsyms=1 op2_deadline=7200
rd=emmcroot0 emmcroot=userdata rootrw=1 emmcrw=1 cs_enforcement_disable=1
unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 op2fb_client_ui=1
op2_tapms=30000 op2_userlog=0
```

Key boot-args: `op2_deadline` (seconds to self-reset; 2h runs need 7200 —
20 min kills the boot mid-way), `op2fb_client_ui=1` (display pipeline),
`op2_tapms` (synthetic touch cadence; enables periodic HOME + swipe for
wake/unlock), `op2_threaddump=0` (disable thread snapshots),
`syssleep=1` (re-allow system sleep; default is hakios-style always-on).

Bake a userdata variant (hunks + daemon parks) by mounting a DMG copy RW on
macOS (`diskutil image attach`), applying `tools/hunk*.py`, moving unwanted
`LaunchDaemons/*.plist` to `LaunchDaemons.disabled/`, ejecting, and flashing.
Current recipe (`trim8`): fixA + hunkQC + hunkCV, `com.apple.lskdd` parked.

## Tool catalog (`tools/`)

| Script | Purpose |
|---|---|
| `build-xnu.sh` | Incremental XNU build for `DEVELOPMENT_ARM64_MSM8994` |
| `mkadt.py` | Apple Device Tree generator (cpu0-only, i2c2, N56AP identity) |
| `mkpayload.py` | Mach-O + ADT + cmdline → `OP2XNU01` payload (`BOOT_LINE_LENGTH` 256) |
| `flash_userdata.py` | Stream userdata DMG to `userdata` partition, arm recovery cookie |
| `mkop2root.py` | Root/USB personality helpers |
| `hunkQC.py` | QuartzCore commit NULL-guard (trampoline; backboardd/SB app-render crash) |
| `hunkCV.py` | CoreVideo NULL-dict guard (1-slot `cbz`; SB min38 SEGV) |
| `op2slide.py` | Per-run dyld cache slide via dual crash anchors |
| `op2sym.py` | Runtime PC → dylib + file offset (needs slide; validated) |
| `disas_cache.py` | Disassemble cache bytes at a static VA (`OP2_DYLD_CACHE` env) |
| `dyld_extract.py` | List/extract/attribute dyld shared-cache images |
| `trie.py` | Dump Mach-O dyld export tries |
| `op2calls.py`, `op2methods.py` | Call/method survey helpers |
| `postrun.sh` | Run-log census helper |
| `mine_fresh.sh` | Fresh-image mining helper |
| `test_tweak_graphics.py` | Graphics tweak test |

`shim/build.py` builds the LK shim + boot image. **Both `--payload` and
`--output` are required** — defaults point at the stock ramdisk and
`out/shim-boot.img`, which silently boots a stale image (bit us in loop-21).

## Hunk catalog (dyld-cache / binary guards)

All hunks are verify-before-write, bake-time patches for NULLs that only
occur without a GPU. See `research/hunkCV-sb-crash.md` for the full
root-cause methodology (slide → symbolicate → disassemble → guard).

| Hunk | Site | Fix |
|---|---|---|
| hunk1 (in fixA) | dyld cache | Baseline cache patch (kept; hunk2/3 reverted) |
| hunkQC | QuartzCore commit+0x14 | `ldr`→`bl` trampoline; NULL commit takes existing graceful path |
| hunkCV | CoreVideo+0x18fd8 | 1-slot `mov`→`cbz`: skip CF apply on NULL dict (pre-zeroed out-param + existing bail) |

## How-it-works (60 seconds)

LK boots the shim → shim loads the XNU payload → XNU (MSM8994 board:
GICv2, 19.2 MHz timer, SDHCI eMMC, I2C digitizer S3320, MDSS framebuffer)
mounts HFS+ root → launchd → SpringBoard/backboardd render to the panel.
No GPU: Metal returns NULL, guarded per call-site by hunks. No SMP yet
(`cpus=1`; secondary bring-up via Qualcomm ACC, not PSCI). Sleep is vetoed
hakios-style (panel must never sleep); synthetic taps + periodic HOME/swipe
drive wake/unlock unattended. Every run self-resets at `op2_deadline` and
leaves a 260 KB ramoops ring: per-minute display timeline, eMMC stats +
fail ring, exit ring with user stacks, and thread snapshots with fp-chains.

## Docs index

- [docs/STATUS.md](docs/STATUS.md) — current state, proof, issues, roadmap
- [docs/OP2_KERNEL_CHANGES.md](docs/OP2_KERNEL_CHANGES.md) — XNU change index
- [docs/HOW-IT-BOOTS.md](docs/HOW-IT-BOOTS.md) — boot flow
- [docs/HACKS.md](docs/HACKS.md), [docs/CREDITS.md](docs/CREDITS.md) —
  workaround log, attributions
- [docs/RESTORE.md](docs/RESTORE.md) — unbrick / restore reference
- [docs/milestone-*.md](docs) — M3/M4 verification reports
- [NOTES.md](NOTES.md) — Day-0..M6 hardware + bring-up log
- `research/*.md` — design notes (sleep gate, eMMC recovery, hunkCV, SMP…)

## Roadmap

1. Eyes validation: lockscreen vs home (loop-26 reproduces loop-25 + full TL)
2. AssistiveTouch/Home (AXS prefs round 2; physical buttons unwired)
3. SMP bring-up (ACC/APCS secondaries; parked after run134 hang)
4. Speed: codesign-page fast path, daemon trim extension
5. Stability soak: multi-hour runs, journal-proof eMMC (write-retry landed)

## Credits

[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) (method),
[oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel)
(sleep-gate + MiniFB lessons), Apple OSS XNU 4903.270.47, checkra1n/pongoOS
research corpus. See [docs/CREDITS.md](docs/CREDITS.md).

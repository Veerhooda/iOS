# iOS on OnePlus 2

Real **Apple iOS 12.5.7 running on a OnePlus 2** (`ONE A2003`, Snapdragon
810 / MSM8994): XNU with a custom MSM8994 board port, real HFS+ root from
eMMC, launchd, SpringBoard, touch, and a working display pipeline —
including user-verified AssistiveTouch and home-screen interaction.

Method inspired by [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)
(Redmi Note 10 Pro), re-engineered for MSM8994: GICv2 instead of GICv3, 4K
pages (iPhone 6 / 4K-native generation), eMMC instead of UFS, and Qualcomm
Little Kernel `boot.img v0` instead of a custom loader.

**Status (2026-11-06): it runs.** Latest 2h validation: 36,250 display
presents, 10 compositor surfaces, lit panel, SpringBoard + backboardd alive
from boot, 2.59M eMMC reads with zero failures, no crashing daemons — plus
hands-on confirmation (AssistiveTouch on, home screen usable, occasional
app crashes). Full evidence: [docs/STATUS.md](docs/STATUS.md).

## Quickstart

Prerequisites: macOS host, Xcode 10.3 toolchain, Homebrew LLVM, `adb` /
`fastboot`, a OnePlus 2 in TWRP (partitions backed up), and a prepared
iOS 12.5.7 userdata image.

```bash
# 1. Kernel (incremental; ~2-10 min)
./tools/build-xnu.sh
cp xnu/xnu-4903.270.47/BUILD/obj/kernel.development.msm8994 \
   out/kernel.development.msm8994

# 2. Device tree + payload + boot image (both flags required)
python3 tools/mkadt.py out/op2.adt
python3 tools/mkpayload.py out/kernel.development.msm8994 out/op2.adt \
    out/op2-hardware-payload.bin --cmdline "<cmdline below>"
python3 shim/build.py --payload out/op2-hardware-payload.bin \
    --output out/xnu-boot.img

# 3. Flash userdata (device in TWRP/recovery, adb up)
python3 tools/flash_userdata.py out/ios12-userdata-fixA.dmg

# 4. Boot; it auto-resets to recovery at op2_deadline
adb reboot bootloader; sleep 12
fastboot boot out/xnu-boot.img

# 5. Pull the verdict (260 KB persistent ramoops ring)
adb pull /sys/fs/pstore/console-ramoops-0 logs/runNNN_ramoops.txt
```

Reference cmdline (≤250 chars; always verify the bytes in the image):

```text
-v debug=0x10014e serial=3 cpus=1 keepsyms=1 op2_deadline=7200
rd=emmcroot0 emmcroot=userdata rootrw=1 emmcrw=1 cs_enforcement_disable=1
unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 op2fb_client_ui=1
op2_tapms=30000 op2_userlog=0
```

Notes that will save you hours:

- Give it time: boot-to-UI takes 30–80 min on one core. A 20-minute
  deadline kills the boot mid-way and looks exactly like a hang.
- `python3 shim/build.py` with no flags silently builds a *stale* image
  (default ramdisk + `out/shim-boot.img`). Always pass both flags.
- `op2_tapms` enables synthetic taps plus periodic HOME and swipe-up, so
  wake/unlock happen with nobody watching.
- Bake userdata variants by mounting a DMG copy RW on macOS, applying
  `tools/hunk*.py`, parking daemons under `LaunchDaemons.disabled/`.

## How it works

LK boots the shim → the shim loads the XNU payload → XNU (MSM8994 board:
GICv2, 19.2 MHz timer, SDHCI eMMC, S3320 I2C digitizer, MDSS framebuffer)
mounts the HFS+ root → launchd → SpringBoard/backboardd render to the
panel. No GPU (Metal returns NULL; guarded per call-site by cache patches
called *hunks*), no SMP yet (`cpus=1`), sleep vetoed hakios-style because
a slept panel never wakes. Every run self-resets on schedule and leaves a
ramoops ring with a per-minute display timeline, eMMC stats + fail ring,
process exit ring with user stacks, and thread snapshots.

Key engineering wins: CoreVideo NULL-dict guard that stopped the SB min38
crash (`hunkCV`), eMMC abort+retry that took failures to zero, MobileAsset
XPC hang diagnosis via symbolicated SpringBoard stacks, and the
lskdd/mobileassetd daemon triage. Details in `research/` and
[docs/STATUS.md](docs/STATUS.md).

## Repository layout

- `tools/` — build, pack, flash, and analysis scripts (hunks, slide +
  symbolication pipeline, cache disassembler/extractor, run census).
- `shim/` — bare-metal ARM64 Little Kernel shim.
- `docs/` — status, boot flow, kernel-change index, milestones, restore.
- `research/` — design notes, postmortems, bring-up plans.
- `NOTES.md` — Day-0…M6 hardware and bring-up log.
- `xnu/` — XNU source tree (local only, see below).

The kernel tree (`xnu/xnu-4903.270.47`, branch `msm8994`) lives next to
this checkout and is indexed in
[docs/OP2_KERNEL_CHANGES.md](docs/OP2_KERNEL_CHANGES.md). Build outputs,
disk images, logs, and device dumps stay out of git.

## Docs

- [docs/STATUS.md](docs/STATUS.md) — current state, proof, issues, roadmap
- [docs/OP2_KERNEL_CHANGES.md](docs/OP2_KERNEL_CHANGES.md) — kernel index
- [docs/HOW-IT-BOOTS.md](docs/HOW-IT-BOOTS.md) — boot flow reference
- [docs/HACKS.md](docs/HACKS.md) · [docs/CREDITS.md](docs/CREDITS.md) ·
  [docs/RESTORE.md](docs/RESTORE.md) — workarounds, credits, unbrick
- [NOTES.md](NOTES.md) — hardware facts and early bring-up log

## Roadmap

Eyes validation of lockscreen-vs-home → AssistiveTouch/Home polish →
8h stability soak → SMP bring-up → speed (codesign fast path, trim) →
app-render hardening.

## Credits

[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) (method),
[oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel)
(sleep gate + framebuffer lessons), Apple OSS XNU, checkra1n/pongoOS
research. Full list: [docs/CREDITS.md](docs/CREDITS.md).

# OnePlus 2 iOS port: context for Claude Code

Read first: docs/AUDIT-2026-11-11.md (what was wrong + fixes), docs/GPU-PLAN.md (next work).
docs/HANDOVER_KNOWLEDGE_BASE.md is the PREVIOUS agent's handover: useful facts, but several
claims were wrong ("0 crashes", "verified" milestones driven by a scripted fake user).

## Hard safety rules
- Volume/Power buttons are broken **on our unit** — never rely on them here. A unit with working
  buttons is fine (they behave normally); the BCB-cookie + deadline rules below apply either way.
- Never `fastboot flash` anything. Boot only with `fastboot boot out/xnu-boot.img`.
- Keep the BCB cookie armed (tools/boot-daily.sh does it) so every reset lands in TWRP.
- Every boot image must have op2_deadline (the only escape from a hang; HW watchdog strands the phone).
- Don't touch CPU voltage beyond the stock values in op2_cpu_boost (APC0 1.145 V, MX super-turbo).
- Kernel source under xnu/ is gitignored: back up before editing (kernel-backup/), or track with git add -f.

## Loop
    bash tools/round.sh            # build + RAM-boot + wait for reset + save log (DEADLINE=900 default)
    logs/last-summary.txt, logs/daily-*.log   # results
    bash tools/rootfs-journal.sh   # rebuild + flash journaled rootfs from out/ios12-userdata-fixA.dmg
Build errors land in logs/last-build-errors.txt. kprintf does NOT reach ramoops (no serial=3);
use printf/IOLog for anything you need to see.

## State (2026-11-13; see docs/SETUP.md for replication, docs/USB-CONSOLE-PLAN.md stages 2-3)
- Quad-core SMP (cpus=4); thermal thread: TSENS, throttle at 70 C, cutoff 90 C (logs show ~768 MHz at low 60s C).
- eMMC CMD18 multi-block reads with self-test; 0 failures so far.
- Double-buffered panel, SwapWait, immediate present; ~6 ms CPU downsample 1242x2208->1080x1920.
- Home = touch starting on bottom 40 px strip (centre third), consumed; sent once via OP2Buttons.
- Synthetic input: op2_autoinput=1 (wake-only). Rootfs journaled, synced before deadline.
- USB serial console (CDC-ACM, live kernel log) + root dash shell on /dev/op2usb with job control
  (d_type D_TTY: iOS 12 isatty checks FIODTYPE, not TIOCGETA) + toybox applets (/usr/local/bin and /bin).
- Composite CDC-ECM: phone ue0 192.168.7.2/24, Mac 192.168.7.1/24, ping verified; SSH exec + scp -O +
  pubkey auth verified (dropbear, host keys via -R, /tmp/dropbear.log on phone).
- GPU G0-G3 pass; G4d/G4e SMMU + pvtmem fix; G5 metal=3 composites home/lock/dock on the Adreno
  (see docs/GPU-PLAN.md). OrangeTree game still blank (SpriteKit pipelines return NULL in the shim) - open.
- Open crashes: logd SIGBUS/SIGSEGV loop (stack probe), Spotlight extensions SIGABRT loop,
  occasional SpringBoard SIGSEGV.
- Reference for GPU: hakios (MIT) github.com/oosexgang/hakios-xnu-kernel (Metal driver,
  AIR->NIR front end tools/mesa/air). Mesa 25.0.x needed (ir3 for chip 0x04030002).

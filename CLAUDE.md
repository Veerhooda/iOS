# OnePlus 2 iOS port: context for Claude Code

Read first: docs/AUDIT-2026-11-11.md (what was wrong + fixes), docs/GPU-PLAN.md (next work).
docs/HANDOVER_KNOWLEDGE_BASE.md is the PREVIOUS agent's handover: useful facts, but several
claims were wrong ("0 crashes", "verified" milestones driven by a scripted fake user).

## Hard safety rules
- Volume/Power buttons are broken. Never rely on them.
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

## State (2026-11-11, round 5)
- CPU 1536 MHz (op2_cpu_boost, deferred 20 s after FB start), measured on every OP2 TOP line.
- eMMC CMD18 multi-block reads with self-test; 0 failures so far.
- Double-buffered panel, SwapWait, immediate present; ~6 ms CPU downsample 1242x2208->1080x1920.
- Home = touch starting on bottom 40 px strip (centre third), consumed; sent once via OP2Buttons.
- Synthetic input: op2_autoinput=1 (wake-only). Rootfs journaled, synced before deadline.
- GPU G0-G3 pass. G4d: SMMU translation works (boot-arg gpu_xl=1, off by default, 4 KB identity pages, CB0):
  stray GPU writes fault + are dropped. NEVER use SMMU ATS (ATS1PR): it resets the SoC. G4e: the G4c stray write
  was dead private-memory stores past fd4's fixed 8 KB buffer; tools/mesa-op2/ir3-fd4-pvtmem.patch drops them, and
  the full-size CA uber draw is now pixel-correct with 0 faults (docs/GPU-PLAN.md G4d/G4e).
- G5a: CA's Metal use traced; op2mtl.dylib (userland/op2mtl, Mesa fd4 behind a C API, built by tools/mesa-ios.sh)
  draws CA's own vertex+fragment pipeline on the phone, geometry verified (docs/GPU-PLAN.md G5a-1). Rootfs has
  slots for it (tools/rootfs_g5.py). Next: the OP2Metal shim onto op2mtl, then metal=2 boots. G0+G1 (2026-11-11): ME runs, TZ SMMU config via SMC, MEM_WRITE fences, hang recovery; userland
  CP fill/copy (userland/op2gpu, probe via tools/deploy-probe.sh as tzinit). G2: every CA pipeline the
  phone asked for compiles for the A430 (tools/mesa-host.sh, tools/ca-compile-captured.sh). G3: Mesa fd4 draws
  on the phone, pixel exact (tools/mesa-ios.sh, job com.op2.air-run in xpcd_cache). See docs/GPU-PLAN.md.
- Open crashes: logd SIGBUS/SIGSEGV loop (stack probe), Spotlight extensions SIGABRT loop,
  occasional SpringBoard SIGSEGV. GPU: nothing drawn yet; see docs/GPU-PLAN.md.
- Reference for GPU: hakios (MIT) github.com/oosexgang/hakios-xnu-kernel (Metal driver,
  AIR->NIR front end tools/mesa/air). Mesa 25.0.x needed (ir3 for chip 0x04030002).

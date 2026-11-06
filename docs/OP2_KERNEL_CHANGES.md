# OP2 kernel changes (`xnu/xnu-4903.270.47`, branch `msm8994`)

The XNU tree is a **nested repo, not pushed here** (Apple source; remote is
`apple-oss-distributions/xnu`). Base: `494ffe1` (pristine xnu-4903.270.47).
HEAD at last inventory: `921f634`. This file indexes the OP2 work so the
tree can be audited and reconstructed.

## Committed OP2 stack (oldest → newest)

- `12623e4` arm64: custom MSM8994 (OnePlus 2) board config
- `864a47b` … `8b29b02` `b73ad17` `f511b59` `2304585`: Xcode 10.3/Apple
  Silicon build fixes, PL011 + Ramoops serial, PS_HOLD reset on panic,
  IMEM recovery cookie, 4K page enforcement, memory diagnostics
- `615ac4a` M3: in-kernel libpthread, corecrypto null-guards, platform
  expert matching, root discovery
- `94dfb07` M4: staged built-in hfs-627.100.6 with xnu-4903 shims
- `9c526b9` M4b: GICv2 interrupt controller + ARM generic timer ticks
- `dab30c6` M5: `op2_emmc` block device + persistent root mount
- `d59391e` M6: AppleKeyStore, IOWatchdog, redundant VFS mount bypass
- `921f634` concise syscall tracing to conserve ramoops

## Uncommitted OP2 work (in working tree; functional index)

Display + input (new files):
- `iokit/Kernel/arm/OP2Framebuffer.cpp`, `OP2Surface.cpp/.h`,
  `apple_logo.h`: MDSS framebuffer, client-UI path, presents/kicks/surface
  counters, per-minute timeline sampler, swap stash, notification gate.
- `iokit/Kernel/arm/OP2I2C.cpp`, `OP2Digitizer.cpp`: S3320 touch (PDT walk,
  F01 init, parent+child HID framing), one-shot setup script + periodic
  autotap/HOME/swipe-up (`op2_tapms`).

Diagnostics (new file):
- `osfmk/arm64/op2_snapshot.c`: SB/backboardd/mobileassetd thread snapshots
  (regs + mach-msg header + fp-chain stacks + dyld info), anchored pre-reset.

Storage:
- `bsd/dev/op2_emmc.c`: SDHCI driver, TIMEOUT tuning, CMD12 abort, DAT0
  wait, op retry after abort (-2/-3/-4 ×3, reads + writes), STATS/FAILRING.

Power:
- `iokit/Kernel/IOPMrootDomain.cpp`: hakios `gSleepDisabledFlag` policy
  gate + idle-sleep/display-dim vetoes.
- `pexpert/arm/pe_identify_machine.c`: deadline reset + diagnostic dumps.

Process / exec / exit:
- `bsd/kern/kern_exit.c`: atomic exit ring (sig/code/PC/LR/user stacks).
- `bsd/kern/kern_exec.c`, `kern_cs.c`, `op2_amfi.c`, `kern_proc.c`,
  `kern_sig.c`, `kern_descrip.c`, `ubc_subr.c`, `bsd/dev/mem.c`
  (`op2_userlog` gate), `osfmk/arm64/sleh.c` (fatal-trap user-state dump),
  `osfmk/vm/*`, `bsd/vfs/*`, `bsd/op2_hfs/*`: code-sign bypasses, spawn
  policy, crash capture, HFS root fixes, page-fault handling.

Platform:
- `iokit/Kernel/arm/OP2CPU.cpp`, `OP2KeyStore.cpp`,
  `OP2PlatformExpert.cpp`, `IOUserClient.cpp`, `IOPMrootDomain.cpp`,
  `pexpert/arm/pe_serial.c`, config tables + `files.arm64` build lists.

## Rebuild

```bash
./tools/build-xnu.sh
cp xnu/xnu-4903.270.47/BUILD/obj/kernel.development.msm8994 \
   out/kernel.development.msm8994
```

Preserving this work: commit inside `xnu/xnu-4903.270.47` (local branch
`msm8994`) and/or push the branch to a **fork** — never to
`apple-oss-distributions/xnu`.

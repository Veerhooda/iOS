# Digest: "OnePlus Fastboot Logo Stuck (Nov 01, 2026)"

Source: `History/conversations/OnePlus 2/OnePlus Fastboot Logo Stuck (Nov 01, 2026)/OnePlus Fastboot Logo Stuck (Nov 01, 2026).md`
(4950 lines / ~185 KB, read completely in sequential chunks; verbatim user text + timestamps cross-checked against the companion `.json`, 291 steps.)
Cascade ID `fc1ef775-75e5-45a8-8739-445c378ca1aa`, 2026-11-01T11:36Z → 20:23Z.
Index: `History/conversations/OnePlus 2/_INDEX.md` lists 2 conversations; this is #2 (291 steps). #1 is "Porting iOS Sweet to OnePlus 2 (Oct 21, 2026)" (30670 steps, not in scope).

Project context: XNU `xnu-4903.270.47` (Darwin 18.7.0, iOS 12.5.7 / iPhone7,1 N56AP) port to OnePlus 2 (MSM8994/Snapdragon 810).
Two near-identical working copies exist: `/Users/veerhooda/Downloads/OnePlus 2` (agent cwd) and `/Users/veerhooda/oneplus2`
(different inodes; build scripts hardcode `/Users/veerhooda/oneplus2`). XNU tree is on branch `msm8994`, outer repo on `main` @ `9e05b6d`.

---

## (a) Symptom timeline (with verbatim user reports)

### T1 — 2026-11-01T11:36:02Z — stuck on 1+ fastboot logo, no display output

User (verbatim):

> `can you chech the current status of the program and tell me what might be the reason that there is no display output and the phone is stuck in 1+ fastboot logo`
> `just check and tell dont change anything`

State at the time:

- Latest build `out/xnu-boot.img` (~11.6 MB) + `out/op2-hardware-payload.bin` (~9.8 MB), payload bootargs
  `op2_deadline=120 ... rd=emmcroot0 emmcroot=userdata rootrw=1 emmcrw=1 ...`.
- Milestone 6 achieved: real iOS 12.5.7 userland on eMMC `userdata` partition; `/sbin/launchd` at EL0,
  42+ daemons, Run 34 ran the full 330 s software deadline then intentional panic-reset to TWRP.
- Run 35 ramoops = `cat: can't open '/sys/fs/pstore/console-ramoops-0': No such file or directory` (79 bytes).
- `fastboot devices` empty, `adb devices` empty, no OnePlus/Qualcomm USB device on host (only Frescologic hubs + Dell dock).
- Uncommitted work: `tools/mkop2root.py` gained `tweak_graphics()` (renames `AGXGLDriver.bundle`,
  `AGXMetalA8.bundle` → `.disabled`), `tweak_timezone()`, `tweak_fstab()`.

Agent diagnosis (read-only): no display is an expected architectural constraint —
(1) LK `target_display_shutdown()` powers off MDSS/DSI before jumping to `0x00080000`;
(2) the OnePlus 2 panel is a Command-Mode DSI panel (`14049_JD35695_1080P_CMD_PANEL`, 1080×1920) with internal
GRAM that self-refreshes the last-written 1+ logo with no driver to kick it (`MDSS_MDP_REG_CTL_START` / `0xfd90201c` never sent);
(3) `shim/loader.c` zeroes `boot_args.Video.*` so XNU logs `initialize_screen: No video - forcing serial mode`
(done to protect the Ramoops buffer at `0xAC0BE000` from the 8 MB video surface);
(4) 100% of logs go to Ramoops; the phone is not frozen, it boots launchd and warm-resets to TWRP at the deadline.

### T2 — 2026-11-01T19:12:38Z — stuck on Apple logo, progress bar never moves

User (verbatim):

> `ok that problem is fixed , now can you analyse why it now gets stuck to apple logo and the progress bar does not progress at all, dont change anything just analyse`

What changed between T1 and T2 (user-side/external, NOT by this agent): a full display stack had been built —
`OP2Framebuffer.cpp` (built-in IOMobileFramebuffer on the continuous-splash FB 1080×1920 BGRA @ `0x03401000`),
`apple_logo.h` (194×237 alpha logo), `OP2Surface.cpp` (built-in IOSurface), `OP2Digitizer.cpp`, plus `mkadt.py`
changes (`sanitize_n56ap_node`). Evidence log: `out/hardware-ios12-run61-ramoops.log` (263 KB).

Agent diagnosis: the Apple logo + bar are painted **once** by the kernel with a **hardcoded 58% fill**
(`OP2Framebuffer.cpp`: `fill_x = bx0 + (bar_w * 58) / 100`), then kicked once to panel GRAM — no kernel timer/thread
advances it. Userland cannot advance it either: `PUIProgressWindow ... _renderWithIOSurface 0`;
`CoreAnimation: Software renderer! Simulating crash!` → emergency overlay
`<BKDisplayRenderOverlayAppleLogo level: 2999>`; all 16 swaps submit completely zeroed surfaces
(`surface 3 (1080x1920) is empty (preserving splash)`, `nz <= 10`) so `AppleCLCD::present()` preserves the splash
instead of black-screening; SpringBoard blocked by `com.apple.baseboard.bssqlite Code=15 "locking protocol"` and
`AppleKeyStore error: -536870201` (`0xE00002C7` = `kIOReturnUnsupported`).

### T3 — 2026-11-01T19:46:32Z — progress bar animates but sticks at 100%, nothing after

User (verbatim):

> `the progress bar not moving is fixed but now it gets stuck at 100% and nothing comes after that, can you analyse why? just analyse and tell dont change anything`

What changed between T2 and T3 (user-side): kernel-side progress animation added to `AppleCLCD::vsyncFired()`
(`OP2Framebuffer.cpp`, mtime Nov 2 01:10 local): 0→35% in 0–3 s, →70% by 8 s, →95% by 15 s, →100% by 20 s
(19.2 MHz counter, 19200 ticks/ms), plus `OP2KeyStore.cpp` lock-state change (`0x4` → `0x6` = no-PIN + unlocked-since-boot).
New build `out/xnu-boot.img` 01:13 local. Still analyzed against run61 log (`op2_deadline=240`).

Agent diagnosis: 100% = the 20 s kernel timer reaching its ceiling, not UI readiness. The kernel hand-off gate
(`OP2Framebuffer.cpp` L683–692) requires `best_nz >= 50` non-zero pixels before `fUserlandTookOver = true`;
CoreAnimation still aborts (`Simulating crash!`, overlay level 2999), Surface 3 stays all zeros (`best_nz = 0`),
so the kernel preserves the 100% splash. SpringBoard additionally blocked by read-only-volume errors
(`NSCocoaErrorDomain Code=642 ... volume "/" is read only`, `NSPOSIXErrorDomain Code=30`) and SQLite locking,
logging `SpringBoard is not done booting. Waiting for notification to start AXSB server.`

### T4 — 2026-11-01T20:21:50Z — still stuck at 100%, and no reboot to TWRP this time

User (verbatim):

> `some changes are made but it still gets stuck at 100% and this time it even did not reboot to twrp it was just stuck at the 100% progress bar , dont make any changes just analyse why it is happening`

What changed between T3 and T4 (user-side): POSIX record-locking fixes — `hfs_vnops.c` `err_advlock` →
`lf_advlock` (+`#include <sys/lockf.h>`), `kpi_vfs.c` routes through `lf_advlock`, `kern_descrip.c` `fcntl`
swallows `ENOTSUP`/`EOPNOTSUPP` → 0, `vfs_subr.c` forces `mp->mnt_flag &= ~MNT_RDONLY`; AppleKeyStore query
handling extended; payload rebuilt 01:39 local with `op2_deadline=600` (10 min) and `trace_syscalls=1` — but
`emmcrw=1` and `rootrw=1` **omitted** from the cmdline. No post-change device log was captured in the conversation
(agent still analyzed run61 + the new payload header).

Agent diagnosis (final, 20:23Z): (1) no TWRP reboot because `op2_deadline` went 240 s → 600 s and the HW watchdog is
disabled in `shim/main.c`, so the software timer is the only reset mechanism — the user simply had not waited 10 min;
(2) still stuck at 100% because the locking/KeyStore fixes did not touch the graphics wall: CoreAnimation still
refuses software compositing, Surface 3 stays all zeros, `best_nz < 50` gate still rejects every frame. Side note:
without `emmcrw=1`, `op2_emmc.c` still rejects writes with `EROFS`, so the forced-RW mount cannot actually persist writes.

---

## (b) Every fix the prior agent attempted, and each observed device result

**The prior agent attempted zero fixes in this conversation.** All four user turns explicitly forbade changes
("dont change anything" / "just analyse"), and the workflow trace confirms the agent ran only read-only
operations: `ls`/`git status`/`git diff`/`grep`/`sed`, file views, payload-header decoding, dyld-cache
disassembly (`tools/disas_cache.py`, QuartzCore `0x18544803c`/`0x185448090` ADRP analysis), and ramoops-log analysis.
No file was written, no build was run, no `fastboot`/`adb` command was issued to the device by the agent.

The fixes below were made **outside this agent's turns** (by the user or another session between turns) and are
listed with the device result the user reported for each:

| # | Fix (external, between turns) | Observed device result (user-reported) |
|---|---|---|
| 1 | Display bring-up: `OP2Framebuffer` (splash render + `op2_fb_kick_panel` DSI GRAM kick), `OP2Surface` (IOSurface), `OP2Digitizer`, `mkadt.py` `sanitize_n56ap_node`, GPU bundles disabled via `tweak_graphics()` | T1 1+ logo problem "fixed" → but now stuck on Apple logo, progress bar frozen at hardcoded 58% |
| 2 | Kernel vsync progress animator (0→100% over 20 s) in `vsyncFired()` + `drawProgressBar(pct)`; `OP2KeyStore` lock state `0x4`→`0x6` | Bar now animates → but sticks at 100%, no Lock Screen/Home Screen after; run61 still showed `Simulating crash!`, `best_nz = 0` |
| 3 | POSIX locking: `lf_advlock` in `hfs_vnops.c`/`kpi_vfs.c`, `fcntl` ENOTSUP→0 in `kern_descrip.c`, forced RW mount in `vfs_subr.c`, KeyStore query fixes; payload `op2_deadline=600` + `trace_syscalls=1`, but `emmcrw`/`rootrw` dropped | Still stuck at 100%; device did NOT return to TWRP within the user's observation window (deadline now 10 min) |

Fixes the agent only *discussed* but never attempted: a Qualcomm MDSS/DSI driver in shim/XNU; an
IOMobileFramebuffer emulation / getting CoreAnimation to composite without `AGXAccelerator`; HFS+ `flock`/`fcntl`
semantics fixes (later done externally in fix #3); restoring `emmcrw=1`/`rootrw=1`.

---

## (c) Last known boot state

### What works

- Shim → XNU boot path: LK hands off, shim parses LK FDT + `OP2XNU01` payload, relocates Mach-O to high DRAM,
  builds ADT + `boot_args`, enters XNU. Ramoops logging end-to-end works.
- Kernel hardware drivers: SDHCI/eMMC probe + GPT, GICv2, ARM generic timer, `OP2CPU`, `OP2Watchdog` (registered,
  never fires), `IOCoreSurfaceRoot`, `OP2Digitizer` (touch/home events reach SpringBoard; `getElements` works),
  `OP2Framebuffer` (splash render, MDP5 registers programmed, DSI kick to panel GRAM, vsync animator to 100%).
- Full iOS 12.5.7 userland: `launchd`, `fsck_hfs`, `keybagd`↔`AppleKeyStore`, 42+ daemons, `backboardd`
  (creates surfaces id 2/3/4/5, issues swaps), `SpringBoard` (boots, bootstraps, receives HID events, workspace
  scene events) — all confirmed in run61 ramoops.
- Software-deadline panic reset path (`msm8994_panic_reset()` → TWRP) as demonstrated by earlier runs
  (run34 @ 330 s; run61's 240 s deadline).

### What hangs

- Display frozen on the Apple logo + 100% progress bar; never transitions to Lock Screen / Setup Assistant / Home.
- No new device feedback in T4's observation window: no TWRP reboot seen (but deadline was 600 s — likely just impatience,
  unverified whether it would have reset at 10 min).
- No newer-than-run61 ramoops log exists in the conversation, so the effect of fix #3 on-device is unconfirmed from logs.

### Current hypotheses (as of the final agent analysis)

1. **Primary (graphics wall):** iOS 12 QuartzCore hard-aborts software compositing
   (`CoreAnimation: Software renderer! Simulating crash!` — verified by disassembly: the string at VA
   `0x1854efe11` is referenced from `0x18544803c` after a capability check at `0x185448038`, plus
   `Failed to initialize Metal or OpenGLES contexts!` at `0x185448090`). `backboardd` then installs the
   `level: 2999` emergency Apple-logo overlay and submits all-zero surfaces, so the kernel's
   `best_nz >= 50` hand-off gate (`OP2Framebuffer.cpp` L683–692) rejects every frame forever. Requires either a
   real compositing path (GPU support / CPU-rendered pixels into the surface) or bypassing the gate.
2. **Secondary (SpringBoard boot completion):** read-only root (`/` and `/private/var`, Code=642/Code=30) +
   SQLite `bssqlite Code=15 "locking protocol"` + KeyStore `0xE00002C7` prevented SpringBoard from finishing boot
   and dismissing the boot overlay. Fix #3 addressed locking/KeyStore in code, but the T4 payload dropped
   `emmcrw=1`/`rootrw=1`, so `op2_emmc.c` still returns `EROFS` on writes — writes still fail despite the forced
   `~MNT_RDONLY` mount flag.
3. **Tertiary (no TWRP reboot):** purely the `op2_deadline=600` lengthening + disabled HW watchdog (PMIC latch-off
   hazard documented in `docs/HACKS.md` §7); no evidence of a new hang/crash blocking the reset path, but also no
   log proving the 10-minute reset fires.

---

## (d) Exact failing prompt context

Last user message (JSON step 261, `2026-11-01T20:21:50.515397Z`), verbatim:

> `some changes are made but it still gets stuck at 100% and this time it even did not reboot to twrp it was just stuck at the 100% progress bar , dont make any changes just analyse why it is happening`

Constraints in force: **analysis only, no changes**. The agent's final response (steps 262–290, ending
2026-11-01T20:23:57Z) attributes the symptoms to `op2_deadline=600` (TWRP delay) + the unchanged CoreAnimation
software-render abort / `best_nz < 50` gate (100% stick), with the `emmcrw`/`rootrw` omission flagged. The
conversation ends there — no user reply, no resolution, no fix applied or verified.

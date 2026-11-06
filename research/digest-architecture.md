# Project Architecture Digest — iOS (XNU) on OnePlus 2 (MSM8994)

Sources fully read: `NOTES.md`, `docs/HOW-IT-BOOTS.md`, `docs/HACKS.md`,
`docs/hardware-xnu-boot-verification.md`, `docs/milestone-3-hardware-verification.md`,
`docs/milestone-4-userland-verification.md`, all 9 milestone `.md` files under
`History/brain_artifacts/fffd8dd8-6f46-4152-8067-daf53e513a06/`
(M3, M4, M4b, M5, M6-launchd, M6-window-server, M7, M8, hardware-xnu-boot).
The 73,158-line conversation log was sampled per instructions (first 400 lines,
keyword grep over milestone/verified/FAILED/stuck/apple logo/boot args/device tree,
targeted regions, final 400 lines).

Target identity throughout: iPhone 6 Plus (`iPhone7,1` / `N56AP`, Apple A8),
iOS 12.5.7 (build `16H81`), XNU `xnu-4903.270.47` (Darwin 18.7.0), native 4 KB pages.
iOS 12 was chosen because it is ARMv8.0-A / 4 KB-page, matching Cortex-A53/A57;
iOS 16 (ARMv8.2 + 16 KB granule) is a hard hardware impossibility on this SoC.

---

## 1. Hardware facts (all verified from live device dumps)

Device: OnePlus 2 `ONE A2003`, Snapdragon 810 / MSM8994 v2.1, 4 GB LPDDR4, 64 GB eMMC 5.0.

### Memory map (from `/proc/iomem` + `live.dts`)
| Range | Size | Use |
|---|---|---|
| `0x00000000–0x062fffff` | ~99 MB | Lower RAM bank (Linux kernel at `0x80000`) |
| `0x06300000–0x07000000` | 13 MB | `memory_hole` carve-out (unmapped) |
| `0x07000000–0x0ca00000` | 90 MB | `modem_mem` carve-out |
| `0x0ca00000–0x0e900000` | 31 MB | `peripheral_mem` carve-out |
| `0x0e900000–0x10200000` | 25 MB | `tzapp_mem` carve-out |
| **`0x10200000–0xf7ffffff`** | **~3.7 GB** | **Main usable bank (XNU `physBase`, ADT `dram-base`)** |
| `0xac000000–0xac100000` | 1 MB | `ramoops_mem` (pstore; console at `0xac0be000`, 256 KB) |
| `0xac200000–0xac300000` | 1 MB | `param_mem` |
| `0xac400000–0xac500000` | 1 MB | `memory_mtp` |
| `0xabf00000` (+1 MB) | 1 MB | XNU panic-log PRAM carve (`PLATFORM_PANIC_LOG_PADDR`) |

### Display
- Panel: 1080×1920 JDI command-mode DSI (`14049_JD35695_1080P_CMD_PANEL`) with internal GRAM + self-refresh.
- Continuous-splash reservation: `0x03401000`, 34 MB (`cont_splash_mem@0`); M8 MDSS work binds the VIG0 pipe at `0x03400000` (see note in §5).
- 32 bpp, stride **4352** bytes (1080×4=4320 padded for 128-byte alignment), double-buffered virtual 1080×3840.
- LK calls `target_display_shutdown()` before kernel jump and does **not** pass continuous splash (`mdss_mdp_splash_parse_dt: splash mem child node is not present`), so DRAM writes alone never reach the panel — a DSI kickoff is required (solved in M8).

### Interrupt controller: ARM GICv2 (`qcom,msm-qgic2`)
- Distributor `0xf9000000` (4 KB), CPU interface `0xf9002000` (4 KB). Strict MMIO (`GICC_IAR/EOIR/PMR`); no GICv3 system registers exist.

### CPUs
- Cluster 0 (LITTLE): cpu0–3 Cortex-A53 (MPIDR 0x00–0x03); **cpu0 is boot core**.
- Cluster 1 (big): cpu4–7 Cortex-A57 (MPIDR 0x100–0x103).
- Enable method `qcom,8994-arm-cortex-acc`; **no PSCI**. All milestones run `cpus=1` (SMP never attempted).

### Architected timer
- `arm,armv8-timer` @ **19.2 MHz**; virtual-timer PPI 4 → **INTID 20** (QEMU equivalent: INTID 27 @ 62.5 MHz).

### Storage: eMMC 5.0 (`qcom,sdhci-msm`, `sdhc1`)
- HC regs `0xf9824900` (0x1a0), vendor core regs `0xf9824000` (0x800).
- IRQs: SPI 123 (INTID 155) HC, SPI 138 (INTID 170) power. 64 GB `mmcblk0` (40 partitions; `system` = mmcblk0p42 2.5 GB, `userdata` = mmcblk0p43 56.3 GB).

### Reset / debug / misc SoC facts
- `PS_HOLD` `0xfc4ab000 = 0` → clean warm reset (DRAM preserved → ramoops survives).
- IMEM restart cookie `0xfe87f65c = 0x77665502` (`RECOVERY_MODE`) → LK boots straight to TWRP. Full cycle ≈ 10–12 s, zero buttons.
- APPS watchdog `0xf9017000` (32.768 kHz) is **deliberately disabled**: a watchdog bite latches the PMIC into emergency power-off (device vanishes from USB, needs battery pull). All runs are instead bounded by deliberate panic/deadline resets (`op2_deadline=N`).
- FDT passed by LK in x0 at `0x01E00000`; ramdisk relocated to `0x02000000` (allowed up to LK base `0x0F900000` = 217 MB headroom).
- Bootloader: Qualcomm Little Kernel, Android `boot.img` **v0**, page 4096, base 0, tags `0x100`. **Appended-DTB rule**: LK reads the DTB offset from kernel+`0x2C`; any raw ARM64 image must write its own length there or LK fails with `dtb not found`.
- USB IDs: EDL `05c6:9008`, fastboot `18d1:d00d`/`2a70:9011`, ADB `05c6:9025`/`9001`.
- HW RNG (`0xf9bff000`) and PMIC RTC exist but are unused (software HMAC-SHA256 PRNG; fake `IORTC` publish).

---

## 2. Boot-chain stages

```
fastboot boot out/xnu-boot.img
  → LK (aboot @0x0F900000): verify v0 header, copy ramdisk→0x02000000,
    select msm8994 DTB via 0x2C offset, add /chosen initrd props, x0=0x01E00000
  → shim @0x00080000, EL1 (shim/start.S: stack, IMEM cookie 0x77665502)
  → shim/main.c (disable WDT) → shim/fdt.c (parse /chosen)
  → shim/sdhci.c (M5+: reset SDCC → HC mode, clear CLK_PWRSAVE, reset DLL,
      CMD0(F0F0F0F0)→CMD0(0)→CMD1 OCR=C0FF8080→CMD2/3/7/6, 25 MHz, parse GPT)
  → shim/loader.c: parse OP2XNU01 @0x02000000 → relocate segments to 0x10200000+
      → place ADT → build 384-B boot_args (Video zeroed → serial mode)
      → dsb/iciallu/isb → jump to XNU entry (x0 = boot_args PA)
  → XNU: arm_init → arm_vm_init (4 KB pmap) → IOKit (OP2PlatformExpert,
      OP2InterruptController, AppleCLCD/OP2Surface/OP2Digitizer/OP2KeyStore/OP2Watchdog)
      → BSD → HFS+ root mount → /sbin/launchd (EL0) → daemons → backboardd/SpringBoard
  → bounded exit: panic or op2_deadline → IMEM cookie + PS_HOLD=0 → TWRP in ~12 s
      → host pulls /sys/fs/pstore/console-ramoops-0 over adb
```

Console strategy: `loader.c` zeroes `boot_args.Video`, so XNU prints
`initialize_screen: No video - forcing serial mode` and routes `kprintf`
into the 256 KB ramoops ring at `0xac0be000`. This is the primary (and for
M1–M5, only) debug channel.

---

## 3. Payload layout (`OP2XNU01`, `tools/mkpayload.py` + `tools/mkadt.py` + `shim/loader.c`)

- Container magic `OP2XNU01`, carried as the Android ramdisk (`0x02000000`).
- Mach-O segments extracted from the built kernel, relocated to `physBase`
  `0x10200000` at `virtBase` `0xffffff f007000000`, 2 MB aligned; 8 segments
  (M2–M3: `__TEXT/__DATA_CONST/__TEXT_EXEC/__LAST/__KLD/__DATA/__BOOTDATA/__LINKEDIT`),
  9 with the M4 RAM-disk segment appended.
- `memSize` `0x9bd00000` (~2.44 GiB, conservative subset of the 3.7 GB bank).
- Apple Device Tree placed just above the top segment (≈`0x10C4B000`, ~6.6 KB):
  root `N56AP`/`iPhone7,1`/`oneplus,oneplus2`/`qcom,msm8994`; `dram-base/size`;
  `/arm-io/gic` (GICD/GICC PAs + vtimer INTID, parsed dynamically so QEMU differs);
  `/product` (47 props from real `DeviceTree.n56ap.im4p`); `/op2-fbcon` (ramoops
  console geometry); relaxed SIP (`lp-sip0=0x6f`); `/chosen/memory-map` `RAMDisk`
  entry for `rd=md0` boots.
- `struct boot_args` (384 bytes, `BOOT_LINE_LENGTH=256`, `_Static_assert`-checked)
  on the next 4 KB boundary; `topOfKernelData` recorded.
- Representative cmdlines:
  - M2/M3: `-v debug=0x10014e serial=3 cpus=1 keepsyms=1 -use_hwpagesize`
  - M4: `... rd=md0 trace_syscalls=1 cs_enforcement_disable=1 cs_debug=1`
  - M5: `... rd=emmcroot0 emmcroot=system ...`
  - M6+: `... rd=emmcroot0 emmcroot=userdata op2_deadline=50..800 rootrw=1 emmcrw=1 ...`

---

## 4. What is VERIFIED working end-to-end (hardware evidence for each)

All items below booted on the physical `ONE A2003` via `fastboot boot` and
self-returned to TWRP with a ramoops log; QEMU `cortex-a57` matches line-for-line
where noted.

- **M0 — image format & DTB rule.** Stock repack boots Android; raw-`Image` boots
  only with DTB offset written at `+0x2C`; 40 partitions SHA-256 two-pass backed up.
- **M1/M1c — bare-metal shim.** EL1 entry, FDT magic `0xd00dfeed` @ `0x1E00000`,
  C runtime + FDT parse, initrd `0x2000000–0x20C3E6B`, 20 s countdown, ramoops
  round-trip, `PS_HOLD` reset. Display filing: command-mode GRAM + LK
  `target_display_shutdown()` explains why DRAM writes don't appear (deferred to M8).
- **M2 — XNU build & first kernel entry.** IPSW iOS 12.5.7 `16H81` fetched;
  `complzss` kernelcache → 28.2 MB Mach-O; `xnu-4903.270.47` pinned with
  `MSM8994.h` board config (`NO_MONITOR`, `VTIMER_INTID 20`, 19.2 MHz,
  `CPU_COUNT 1`), `MASTER.arm64.msm8994`; `mkadt.py` + `mkpayload.py` +
  `loader.c` pipeline; `xnu-boot.img` (28.6 MB) reaches canonical
  `Unable to find driver for this platform: "N56AP"` panic and warm-resets.
- **M3 — IOKit/BSD/root discovery.** `OP2PlatformExpert` (score 10k),
  in-kernel `libpthread-330.250.2`, corecrypto NULL guards, forced 4 KB pages
  (`vm_page_bootstrap: 602199 free, 12969 wired` = 2,403 MiB managed, 97.9% free),
  PRNG DRBG, DLIL SUM16 PASSED → `IOFindBSDRoot … Waiting on Apple_HFS` +
  bounded `Milestone 3 SUCCESS` panic.
- **M4 — first userland.** In-kernel HFS+ (`hfs-627.100.6` + 10.14 KPI shims),
  4 MB RAM disk `rd=md0`, `/sbin/launchd` PID 1 executes `open→write→exit`
  (syscalls 5/4/1), prints `*** OnePlus 2 userspace: hello from /sbin/launchd
  (PID 1) ***`, return PC `0x10000044c`, 12,416-byte stackshot — QEMU/HW identical.
- **M4b — interrupts & time.** Dynamic GICv2 from DT
  (`GICD_TYPER=0x4ed`, 448 IRQs, 8 CPUs; `GICC_PMR=0xf0`), virtual timer
  10 ms (`init_tick=192000`), 100+ `sleh_irq intid=20 → rtclock_intr` ticks,
  `OP2InterruptController` registered. Fixed: IOKit start order, `sxtw` heap
  truncation (missing `IOMallocZero` prototype), LP64 freq-parse blowup, Group-0
  `AIAR/AEOIR` ack path.
- **M5 — eMMC persistent root.** Shim JEDEC reset (`CMD0(F0F0F0F0)` pre-idle),
  `CMD1 → OCR 0xC0FF8080` first try, 8-bit bus @ 25 MHz PIO; in-kernel
  `op2_emmc` (`bdevsw` major 2, `cdevsw` major 13, `/dev/emmcroot0`,
  GPT lookup by `emmcroot=` name, `EROFS` write guard); `system` partition
  (LBA 1572864, 5,242,880 blocks) → `hfs: mounted rootfs` → PID 1 runs
  from flash. Android `system` image preserved to `backup/system.img.gz` first.
- **M6-launchd — real iOS 12.5.7 userland (Run 6, 263 KB / 9,337 lines).**
  6 GB HFS+ rootfs on `userdata` (mmcblk0p43); unmodified `/sbin/launchd`
  passes every boot task (`fsck_hfs` s1+s2, `init_data_protection`/`seputil`,
  `mobile_obliterator`, `HorizonSetup`, `cc_fips_test` 16/16 in 224 ms,
  `mount_hfs` incl. VFS redundant-rootdev bypass); built-in `AppleKeyStore`
  (`aks_get_system` sel 14 → `MKB_INIT`) + `IOWatchdog`; AMFI/Sandbox/
  Quarantine MAC shims with `CS_PLATFORM_BINARY` preserved; 42+ processes
  (`keybagd`, `xpcroleaccountd`, `fud`, `tzinit`, `FinishRestoreFromBackup`,
  `sysstatuscheck`, `xpcproxy` fleet); 50 s crash-free; `op2_deadline` reset.
- **M6-window-server (Run 23, 120 s).** `backboardd` (PID 56) +
  `SpringBoard` (PID 52, `414x736@3x` main display) + 25-daemon fleet;
  `ProgressUI` loads `apple-logo@3x~iphone.png`; `IOSurfaceRoot` +
  `IOMobileFramebuffer`/`AppleCLCD` swap 1080×1920 BGRA stride-4352 frames
  into panel memory. Fixed: surface-release selector 1 (leak at 65-surface
  cap), selector 49 `SurfaceIsReplaceable=1`, swap-rec size `0x278` + exact
  `rec` ABI offsets (`swapID+0x40`, `surfaceID+0x44+4*layer`, …).
- **M7 — interactive touch (Run 30, 306 s + reset at 330 s).**
  `OP2Digitizer` as `IOHIDEventService` (usage 13/4, `IOHIDLib.plugin`,
  16,432-B shared queue, Mach notify port); byte-exact 132-B digitizer /
  52-B keyboard (`0x0C/0x40` Home) packets; `acquired main display digitizer`
  (sender `0x10000011B`); all **129 injected events dequeued, 0 dropped**
  (taps, swipe-to-unlock path, 3× Home); SpringBoard bootstrap +
  event routers + backlight lock; 7 `AppleCLCD` swaps; scaler/accelerator
  log-spam filtered to fit the 256 KB ramoops window.
- **M8 — physical display (Runs 48/50/51, 120 s each, user-photographed).**
  MDSS VIG0 take-over (32 bpp XRGB8888 `0x236ff`, unpack `0x3020001`,
  stride 4352) + `CTL0_FLUSH=0x20041` / `CTL0_START=1` DSI kick → LK `1+`
  logo replaced by live frames; Run 48: white box + blue bar; Run 50:
  anti-aliased splash + progress; Run 51: **official 194×237
  `apple-logo@3x~iphone.png` + rounded capsule bar** — user confirmed
  ("yes i saw it, it was real") with `user_screen.png` photo. `launchd`/
  `backboardd`/SpringBoard + 25 daemons crash-free; `/private/etc/fstab`
  set `ro` + kernel `EROFS`-on-`MNT_ROOTFS` defense stops `launchd`
  emergency shutdown.

Deliberately still single-core (`cpus=1`), watchdog-off, software-PRNG,
read-mostly storage throughout. Post-M8 conversation tail (Runs 52–80)
covers ongoing bring-up debugging (SpringBoard respawn/`CFRelease(NULL)`
shared-cache patch, stuck-at-100% / stuck-Apple-logo display hand-off gate)
— active work, not new verified milestones.

---

## 5. Open discrepancies / caveats (observed, not resolved here)

1. Framebuffer base is `0x03401000` in NOTES/cont-splash docs but `0x03400000`
   in M8 MDSS/VIG0 material — likely bootloader-pipe vs `cont_splash_mem`
   node; both values appear verbatim in verified logs.
2. `docs/HACKS.md` numbering skips #10 and its last GIC/timer entry is
   unnumbered; content itself is complete.
3. `History/brain_artifacts/.../hardware-xnu-boot-verification.md` is
   byte-identical to `docs/hardware-xnu-boot-verification.md` (verified via diff).
4. `NOTES.md` ends at "Milestone 6 Complete" text; M6-window-server/M7/M8
   verification lives only in the brain-artifact milestone files + conversation log.

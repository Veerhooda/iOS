# Boot-code audit: Apple-logo-hang suspects (OnePlus 2 / MSM8994 shim + XNU)

Date: 2026-11-03. Sources read in full: `shim/start.S`, `shim/main.c`,
`shim/loader.c`, `shim/console.c`, `shim/fdt.c`, `shim/sdhci.c`,
`shim/build.py`, `shim/shim.ld`, `tools/mkadt.py`, `tools/mkpayload.py`,
`tools/mkinit.py`, `tools/mkop2root.py`, `tools/build-xnu.sh`, `tools/adt.py`,
plus `shim/*.h`, `xnu/.../MASTER.arm64.msm8994`,
`pexpert/pexpert/arm64/MSM8994.h`, `pexpert/pexpert/arm64/boot.h`,
`pexpert/arm/pe_init.c:327`, `osfmk/arm64/arm_vm_init.c`,
`logs/run59_ramoops.txt` (early boot) and `logs/run80/81_ramoops.txt` (tails).

Status quo: current code boots to userland on hardware (run59 shows clean
handoff + `GICv2 [TIMER]` + `kernel_bootstrap`; run80/81 tails show launchd,
SpringBoard, digitizer, then the intentional 800 s software-deadline panic).
So this audit is about what *would* hang at the logo if regressed, plus live
latent issues found.

## 1. Exact boot flow

1. Little Kernel (aboot) loads `out/shim-boot.img`: kernel (shim+DTBs) at
   `0x00080000`, ramdisk (OP2XNU01 payload) per header (observed
   `0x02000000..0x02963530`), enters shim at EL1, MMU off, D-cache off,
   `x0` = patched FDT (observed `0x01E00000`), x1–x3 = 0.
2. `start.S:_head` (Linux v0 header: `text_offset 0x80000`, magic `ARM\x64`,
   DTB offset at `0x2C` patched by build.py to shim_len): masks DAIF, writes
   IMEM recovery cookie `0x77665502` to `0xfe87f65c` (only if PC <
   `0x40000000`, i.e. hardware), saves FDT→x19, PC→x21.
3. QEMU only (PC in `[0x40000000,0x50000000)`): copies whole shim to
   `0x50000000`, `ic iallu`, jumps there. Stack: HW `0x00400000` (grows down
   toward `0x00300000`); QEMU `entry+0x380000`. Sets `VBAR_EL1` to shim
   vectors, zeroes `.bss`, calls `shim_main(fdt, entry_pc)`.
4. `shim_main`: `is_qemu = entry>=0x40000000`; ramoops init
   (`sig 0x43474244` at `0xac0be000`, cap 260000 B; QEMU: PL011 char out at
   `0x09000000`). Reads `/chosen linux,initrd-start/end` + `bootargs`.
   If ramdisk magic = `OP2XNU01`: rewrites IMEM cookie, arms/disarms watchdog
   (`wdt=1`/`wdt_en=1` in payload cmdline at `rd+0x48` → `watchdog_arm(30)`),
   runs `sdhci_probe()` (diagnostic only, return value ignored), then
   `loader_boot(fdt)`. Otherwise dumps FDT (`/memory`, `/reserved-memory`,
   `/cpus`), waits 2 s, resets to recovery via PS_HOLD.
5. `sdhci_probe()`: dumps SDCC MMIO, resets Qualcomm core → HC mode, vendor
   tweaks, DLL reset, 400 kHz clock, card init (CMD0/1/2/3/7/6/16/13, RCA=1,
   8-bit, 25 MHz), reads GPT, finds `userdata`, checks ext4 magic. All
   bounded loops; failures only print.
6. `loader_boot`: re-reads initrd bounds, checks magic + `version==1`, checks
   ramdisk-vs-kernel overlap (`kernel_load_end = phys + (kend-virt) + 2 MB`),
   `copy64`s each segment to `vmaddr + (phys-virt)`, zeroes BSS tails, copies
   ADT to `kernel_end+pa_base`, builds 4 KiB `boot_args` page after ADT
   (`top = ba+4K`), `dsb/ic iallu/dsb/isb` (no D-cache clean — relies on
   caches staying off), jumps to `entry_va+pa_base` with `x0 = ba_pa`.
7. XNU (`entry 0xFFFFFFF007288098`, PA `0x10488098` in run59): `start.s` takes
   `x0` = boot_args, builds bootstrap page tables at `topOfKernelData`,
   `arm_vm_init` maps DT/RD/boot_args (`end_kern..top`), `pe_init` sets
   `deviceTreeHead = deviceTreeP` (used as a VA), `initialize_screen` sees
   zero Video → "No video - forcing serial mode", GICv2 + vtimer init from
   ADT, `kernel_bootstrap`, root mount (`rd=emmcroot0`), launchd.

## 2. Every hardcoded address / size / boot-arg / DT property

### 2a. Addresses and sizes (shim + tools + XNU headers)

| Value | Meaning | Source |
|---|---|---|
| `0x00080000` | Shim link/load address (`ENTRY _head`) | shim.ld:5, build.py:120, run59 PC |
| `0x2C` | LK DTB offset word (src `0x1000`, build.py overwrites with shim_len) | start.S:28, build.py:91 |
| `0x80000` | Linux `text_offset`; magic `0x644D5241` "ARMd" | start.S:23,30 |
| `0x03401000` | LK cont-splash framebuffer base | start.S:13, console.h:FB_BASE |
| 1080x1920, stride 4352 (1088*4) | FB geometry (shim console) | console.h |
| `0xac0be000` + 256 KiB (`0x40000`), cap 260000 B | Ramoops console (`sig 0x43474244`, start@4, size@8, data@12) | main.c:10-11, start.S:444, MSM8994.h |
| `0xfc4ab000` := 0 | PS_HOLD reset | start.S:287, main.c:138 |
| `0xfe87f65c` := `0x77665502` | IMEM restart_reason = recovery | start.S:194, main.c:135 |
| `0x00400000` | HW stack top | start.S:247 |
| `0x40000000` | QEMU/HW PC discriminator; QEMU DRAM base | start.S:190, main.c:145 |
| `0x50000000` | QEMU shim relocation target; stack `+0x380000` | start.S:211,251 |
| `0x09000000` | QEMU PL011 console | main.c:51 |
| `0xf9017000` (+0x04 RST, +0x08 EN, +0x10 BARK, +0x14 BITE) | KPSS watchdog; tick = 32765 Hz; arm(30) → bark 30 s, bite 35 s | main.c:112-125 |
| 19.2 MHz (`19200000`) | CNTVCT freq; 5 s = `0x05b8d800` ticks | start.S:421, main.c:94, mkadt.py:41 |
| `0x10200000` + `0x9be00000`, minus 1 MiB panic log → dram-size `0x9bd00000`, managed end `0xABF00000` | Kernel DRAM (low carve-outs end `0x10200000`, ramoops above) | mkadt.py:20-21,96 |
| `0xabf00000` + 1 MiB | Panic-log buffer (`pram` node; `embedded-panic-log-size`) | mkadt.py:96, MSM8994.h |
| `0x01000000` | `arm-io` SOC_BASE; `ranges` maps `0 → SOC_BASE`, size `0x100000000-SOC_BASE` | mkadt.py:27,207 |
| GICD `0xf9000000/0x1000`, GICC `0xf9002000/0x1000`, vtimer PPI4 → intid 20 | GICv2 (MSM8994) | mkadt.py:30-31,42, MSM8994.h |
| QEMU: GICD `0x08000000/0x10000`, GICC `0x08010000/0x10000`, intid 27, 62.5 MHz, DRAM `0x40000000/0x40000000` | QEMU virt ADT | mkadt.py:45-48,93,99 |
| SDHCI `0xf9824900/0x1a0`, CORE `0xf9824000/0x800`, TLMM `0xfd512044` | eMMC host | mkadt.py:38-39, sdhci.c:8-9,45 |
| 200 MHz base clock; init 400 kHz → 25 MHz; OCR `0x40FF8080`; RCA 1; CMD6 `0x03B70200` 8-bit; block 512 | SDHCI bring-up | sdhci.c:240,449,382,414,438 |
| `0x1000` 4 KiB pages; virt_base `__TEXT & ~2M`; overlap slack `0x200000` | Payload layout | mkpayload.py:25-26,126; loader.c:144 |
| Payload header: magic@0, ver/nsegs@8, virt@0x10, entry@0x18, adt@0x20, kend@0x30, phys@0x38, mem@0x40, cmdline@0x48 (256 B), segs@0x148 (32 B each) | OP2XNU01 ABI | loader.c:26-39, mkpayload.py:8-17 |
| `boot_args` 384 B: Revision=2, Version=2, Video=0×6, machineType=0, `deviceTreeP`=ADT_VA, `deviceTreeLength`=u32 size, `bootFlags`=0, `memSizeActual`=mem | xnu-4903 boot_args | loader.c:182-206; boot.h:42-45 |
| `kBootFlagsDarkBoot` bit 0 (unused, flags=0) | boot.h:36 |
| ECID `0x4F50320000000001`, chip-id `0x7000` (A8), `boot-manifest-hash` = sha384("oneplus2 boot manifest") | ADT identity | mkadt.py:50-51,118-119 |
| cpu0: cortex-a53, MPIDR 0, 1.5552 GHz, bus 100 M, mem 1600 M | ADT /cpus | mkadt.py:131-151 |
| `random-seed` 64 B, `display-scale` 3, `asmb lp-sip0 0x6f / lp-sip1 1`, `debug-enabled` 1, `crypto-hash-method sha1`, cert/security-mode words | /chosen | mkpayload/mkadt.py:106-127 |
| `disp0` canvas 1080x1920, compat `op2,display`+`adbe0,t7000`; `op2-fbcon` reg=ramoops, stride **4320**; `wdt no-pmu`; `defaults no-effaceable-storage`; buttons; `options{}`; `product` = sanitized N56AP | ADT nodes | mkadt.py:170-243 |
| `dram-base`/`dram-size` u64; `RAMDisk` u64 pair in `chosen/memory-map` (only with `--ramdisk`) | mkpayload.py:101-114 |
| boot.img: base 0, kernel_off `0x80000`, ramdisk_off `0x1000000`, tags `0x100`, pagesize 4096, header v0 | build.py:113-125 |
| mkinit: user BASE `0x100000000`, SEG `0x4000`, CODE_OFF `0x400`, ad-hoc SHA-256 CD | mkinit.py:26-28 |
| mkop2root: iOS 12.5.7 `16H81`, fstab `/dev/disk0s1 / hfs rw,noatime`, binary patches (mobileactivationd/SpringBoard/dyld cache) | mkop2root.py:27-28,500-477 |
| build-xnu.sh: `xnu-4903.270.47`, Xcode 10.3, MacOSX10.14.sdk, ARM64/DEVELOPMENT/MSM8994, `-j1`, `BUILD_LTO=0` | build-xnu.sh |

### 2b. Boot-args (XNU cmdline)

- Packager default: `-v debug=0x14e serial=3 cpus=1 keepsyms=1` (mkpayload.py:175).
- Observed hardware (run59): `-v debug=0x10014e serial=3 cpus=1 keepsyms=1
  op2_deadline=240 rd=emmcroot0 emmcroot=userdata trace_syscalls=1
  cs_enforcement_disable=1 cs_debug=0 no_interrupt_masked_debug=1
  unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 wdt_test=no_user_checkin op2`
  (run80/81 add `rootrw=1 emmcrw=1`, `op2_deadline=800`, `wdt_test=n`).
- LK→shim cmdline (build.py:108-112): `androidboot.hardware=qcom
  user_debug=31 msm_rtb.filter=0x37 ehci-hcd.park=3
  lpm_levels.sleep_disabled=1 boot_cpus=0-3 loop.max_part=7
  androidboot.selinux=permissive buildvariant=userdebug` (diagnostic only).
- Watchdog gate: payload cmdline containing `wdt=1` or `wdt_en=1` → 35 s
  bite; default path leaves watchdog off (main.c:187-191).

## 3. RANKED suspect list for a hang at the Apple logo

Context: `Video` is all-zero, so XNU prints "No video - forcing serial
mode" and never draws a logo; a "logo hang" on this hardware = frozen LK
splash + serial silence. Ranked by symptom match × likelihood:

1. **Framebuffer handoff (no video by design + stride mismatch).**
   Zero Video means zero on-screen progress — any later stall *looks* like a
   logo hang. Live bug: ADT `op2-fbcon stride` = 4320 (`1080*4`,
   mkadt.py:35) disagrees with the real 4352 (`1088*4`, console.h, XNU log
   `bpr=4352`). Any consumer of the ADT stride paints a skewed image.
2. **deviceTree pointer/length.** `deviceTreeP` is passed as a *VA*
   (`adt_pa - pa_of_va`, loader.c:199), which is correct for this XNU
   (`pe_init.c:327` assigns it straight to `deviceTreeHead`), and `length`
   is truncated to u32. But the DT is only mapped because it sits in
   `end_kern..topOfKernelData` (arm_vm_init.c:1191). If the ADT ever grows
   past the 4 KiB `boot_args` page reservation or is misaligned, XNU faults
   before first kprintf — the classic silent logo hang.
3. **boot_args Revision/Version.** Must be 2/2 (Revision2 added `bootFlags`;
   boot.h:42-45). Any value < 2 shifts XNU's field layout and corrupts
   `virtBase/physBase/deviceTreeP`. Currently correct (loader.c:183-184).
4. **Kernel entry/slide.** `virt_base = __TEXT & ~2M`, PA = VA + (phys-virt),
   chained fixups must be rebases-only (`imports_count==0` asserted,
   mkpayload.py:57). A slide/fixup mismatch jumps into garbage with no
   output; the 2 MB overlap slack (loader.c:144) is the only guard.
5. **MMU/cache assumption at handoff.** Shim runs MMU-off/D-cache-off and
   only does `ic iallu` (loader.c:224) — no D-cache clean. Correct *only* if
   LK really left D-cache off. If LK ever enables it, `copy64` leaves dirty
   lines and XNU reads stale text/ADT. Harden with `DC CIVAC` + `DCCISW`.
6. **Handoff register state.** Only `x0` (ba_pa) is defined; x1+ hold C
   residue, `VBAR_EL1` still points at shim vectors until XNU sets
   `LowExceptionVectorBase`, DAIF is masked (correct — XNU expects IRQ-off
   entry). A stray SError/NMI in the window before XNU's VBAR write lands in
   shim's red-screen panic instead of XNU.
7. **GIC mappings (minimal sizes).** ADT exports GICD/GICC as 4 KiB each
   (mkadt.py:30-31) vs GIC-400's 64 KiB/8 KiB TRM windows. Covers only
   CTLR/TYPER/IIDR/ISENABLER/IAR/EOIR — sufficient today (verified in run59:
   `TYPER=0x...4ed, cpus=8`), but any XNU path touching GICD_ITARGETSR0–7
   aliases or GICC_DIR faults. `virtual-timer-intid` 20 (PPI 4) must match
   `MSM8994_VTIMER_INTID`.
8. **Generic timer / CNTFRQ.** ADT `timebase-frequency` 19.2 MHz drives all
   XNU deadlines (mkadt.py:144-150); `CNTFRQ_EL0` is never programmed by the
   shim. A mismatch hangs `clock_init`/calibration with serial stopping
   right after "Darwin Kernel Version".
9. **Watchdog bite on slow boot.** Default off (safe). With `wdt=1`, bite at
   35 s (main.c:112-125) — full boot to SpringBoard takes minutes, so a
   watchdog-enabled run *always* resets mid-boot and looks like a logo hang
   followed by a reboot loop.
10. **ADT completeness.** `pe_arm_init_debug: failed to find
    cpu-debug-interface` (run59, non-fatal today); single `cpu0` only
    (`cpus=1`); sanitized N56AP `product` node depends on
    `research/ipsw-12/n56ap.adt` existing at pack time (mkadt.py:227-228 —
    silently omitted if missing, which could break platform matching).
11. **SDHCI probe blocking boot.** Currently safe (return ignored,
    main.c:195; all loops bounded). Would become a logo hang only if a future
    edit makes `loader_boot` depend on its result or adds an unbounded poll.
12. **QEMU-only: `uart0` ADT `reg` is absolute (`0x08000000`, mkadt.py:187)
    while `gic` is SOC-relative** — inconsistent with the `ranges` translation
    that yields correct GIC PAs. Any XNU `arm-io` child parser that applies
    `ranges` uniformly computes a wrong UART address under QEMU.

## 4. Minor findings (fix opportunistically)

- start.S:9 comment says shim entry `0x00008000`; real address is `0x00080000`
  (shim.ld:5, run59). Stale comment.
- start.S:28 `0x2C = 0x1000` is dead: build.py:91 always overwrites it with
  the true padded shim length. Keep in sync or note it.
- Ramoops 260000 B cap (start.S:447, main.c:56) ring-overwrites: newest logs
  (run80/81) preserve only userland tails; early-boot evidence must be caught
  in smaller runs (run59 pattern) or the cap raised.
- `machineType = 0` (loader.c:198) is fine — arm64 XNU identifies via ADT
  (`Device: N56`, `iPhone7,1` in panics), not this field.
- Managed DRAM ends exactly at `0xABF00000` = panic-log base; ramoops
  `0xac0be000` is outside managed range — no allocator collision (confirmed
  via run59 `real_avail_end=0xabf00000`).
- `copy64` over-reads up to 7 B past `file_size` into inter-piece padding
  (loader.c:81) — safe because mkpayload 16-B aligns pieces, but the
  invariant is implicit; assert it.

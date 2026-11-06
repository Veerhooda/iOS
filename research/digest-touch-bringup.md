# Touch bring-up research digest (run112+)

## Hardware truth (OnePlusOSS android_kernel_oneplus_msm8994, branch 5.1.1,
board 14049_HW_14 = OnePlus 2, SoC MSM8994v2.1)

- Touch controller: **Synaptics S3320**, RMI4, **I2C addr 0x20**
  (`arch/arm64/boot/dts/14049_HW_14/msm8994-mtp.dtsi`, `&soc { i2c@f9924000
  { synaptics-rmi-ts@20 { compatible = "synaptics,s3320"; ... } } }`)
- **BUS = BLSP1 QUP2**: `i2c_2: i2c@f9924000` in msm8994.dtsi:
  reg 0xf9924000/0x1000, GIC SPI **96**, 400 kHz out / 19.2 MHz in,
  clocks gcc_blsp1_ahb_clk (iface) + gcc_blsp1_qup2_i2c_apps_clk (core).
- I2C pins: **GPIO 6 = SDA, GPIO 7 = SCL**, func 3, 2 mA, no pull
  (msm8994-pinctrl.dtsi `pmx_i2c_2: qcom,pins = <&gp 6>, <&gp 7>;
  qcom,pin-func = <3>`).
- ATTN/IRQ: **GPIO 96**, falling edge (flags 0x08 = IRQF_TRIGGER_FALLING).
  Reset: **GPIO 31**. 1v8 enable: **GPIO 74**. (wakeup/id gpios commented out.)
- VDD 2v8: `pm8994_l22` regulator. **Power state at our boot is UNKNOWN**
  (no RPM/SPMI driver). First I2C probe answers it: ACK = powered.
- **TLMM**: `pinctrl@fd510000`, reg 0xfd510000/0x4000, GIC SPI **208**,
  146 GPIOs (`msm8994-pinctrl.dtsi`, compat qcom,msm-tlmm-8994).
- **GCC**: `qcom,gcc@fc400000`, reg 0xfc400000/0x2000. Offsets
  (drivers/clk/qcom/clock-gcc-8994.c):
  BLSP1_AHB_CBCR = 0x05C4, BLSP1_QUP2_I2C_APPS_CBCR = 0x06C8,
  BLSP1_QUP2_I2C_APPS_CMD_RCGR = 0x06E0.
  CBCR: bit0 = enable, bit31 = CLK_OFF status (poll clear after enable).
  RCG: program CFG for XO/19.2 MHz, set UPDATE in CMD, poll done.
- Panel 1080x1920 (dsi_jdi_1080_vid) matches OP2Digitizer 1080x1920.
- Fingerprint FPC1020 on SPI (irrelevant).

## Reference sources (fetched to /tmp/op2touch/)

- `i2c-qup.c` (1880 lines) — exact QUP v2 programming: reset, config,
  TX/RX FIFO, BAM vs PIO mode. We use **PIO/interrupt mode**, no BAM/DMA.
- `synaptics_dsx_i2c.c` + `synaptics_dsx_core.c/h` — Synaptics RMI4 stack:
  PDT scan from 0xEE down, F01 (device control), F11 (2D touch) setup.
- `synaptics_driver_s3320.c` (3552 lines, OP2-specific) — power/reset
  sequence, ATTN handling, F11 12-bit data decode, max_x/max_y query.
- `clock-gcc-8994.c` — CBCR/RCGR offsets + RCG parent tables.
- `op2-mtp.dtsi`, `op2-msm8994.dtsi`, `op2-pinctrl.dtsi` in /tmp.

## RMI4 essentials (from dsx core + S3320 driver)

- PDT: 6-byte function descriptors from 0xEE downward (page 0); each gives
  query_base, cmd_base, ctrl_base, data_base, intr_src_count, fn number.
- F01 data[0] bit0 = device status; F01 ctrl[0]: bit7 sleep-enable,
  bits0-2 sleep mode (0 = normal). Device boots awake; no FW update needed.
- F11_2D_data: per-finger 5 bytes (X[11:4], Y[11:4], XY[3:0], WX/WY, Z).
  Number of fingers from F11 query 1 (bits0-2 = max fingers - 1... exact
  mask in dsx core: query1 & 0x07 + 1, capped by driver).
- Interrupt: ATTN (gpio96) falls -> read F01 data + F11 data block in one
  pass, report fingers, done. No explicit IRQ ack needed on device side
  (level clears when status read); TLMM edge-triggered is fine.
- Max coords: F11 ctrl 6-9 (sensor_max_x/y, 12-bit each) -> scale to 1080x1920.

## What already exists in XNU tree

- `OP2Digitizer.cpp` (IOHIDEventService, Built-In): posts synthetic
  TOUCH_DOWN/UP (92 events in run111) but backboardd/hidd never consume
  them (hidd never spawns). Real multitouch path must feed the same
  dispatchRelativePointerEvent path once validated.
- `OP2InterruptController` (+ GIC SPI support already used for QUP-class
  devices?) — check before wiring gpio96/QUP96/TLMM208.
- No TLMM/GPIO, no QUP/I2C, no SPMI/RPM drivers. GCC clocks: check whether
  any existing driver (UART? SDHCI? MDSS?) programs GCC; copy its CBCR idiom.

## Bring-up plan (each step boot-verified via ramoops)

1. **OP2GPIO** (TLMM @ 0xfd510000): func/pull/drive per-pin, input read,
   IRQ route gpio96 -> TLMM summary -> GIC 208. Verify: gpio96 reads HIGH
   (idle, pulled up) if touch powered; gpio6/7 switch to func 3.
2. **OP2I2C** (QUP2 @ 0xf9924000, SPI 96): GCC clocks on, QUP reset/config
   400 kHz PIO, probe 0x20. Verify: ACK + F01 query manufacturer = 0x01.
   If NACK -> power problem (pm8994_l22) -> SPMI/RPM work or LK handoff.
3. **OP2Synaptics** (RMI4): PDT scan, F01 wake, F11 config, ATTN-thread
   reads finger block, converts to digitizer reports, feeds OP2Digitizer.
   Verify: coordinates in ramoops on finger touch.
4. Remove synthetic tap sequence; validate real taps move UI (hidd/backboardd
   path) — may need HID report format fixes once real events flow.

## Risks

- **Power**: pm8994_l22 may be OFF (Android enables at probe). No SPMI/RPM
  driver exists. Fallback: check LK/aboot PMIC defaults (many rails default
  ON); else minimal SPMI write to enable l22 (SPMI base 0xfc4c0000 on 8994
  — verify from DT).
- **Clocks**: RCG parent index for XO must come from clock-gcc-8994.c tables
  (read exact struct, do not guess).
- **QUP state**: aboot/LK leaves QUP2 in reset; full init required (unlike
  UART which LK leaves running).
- **hidd**: even with real events, iOS 12 hidd never spawned in run111 —
  event consumption path may need separate fix (IOHIDSystem matching).

## Delivery debugging (run119-122) — SOLVED run122

- Events reached backboardd (EventStatistics) but UI dead. Compared packets
  against Apple IOHIDFamily-1090 (iOS 12) source: fixed options
  (drop collection bit on transducer: down=0x30001/up=0x01), transducerType
  finger=2, identity=2, stable transducerIndex=1, pressure/orientation,
  queue header (options/attributeLength=0), eventMask DOWN/MOVE=0x07 UP=0x03.
- Still dead (run120/121). Disassembled backboardd: service attaches as main
  digitizer (builtIn:1, run116 log), filter passes, but arbiter
  (0x100033708) requires >= 1 CHILD event, else silent drop (0x100033f80).
- Fix: collection framing — header eventCount=2 + parent
  (absolute|collection, centroid, masks) + child (finger, depth 1).
- run122 USER-VERIFIED: taps, swipes, widgets, keyboard all work.
- New: backboardd SEGV x2 in QuartzCore+0x30648 (NULL deref, `ldr x0,[x0]`
  after call returns NULL) — only in run122 (first interactive run).
  Jupyter: touch-enabled app launches stress the compositor; trigger TBD.
- Tap loop set to single pass (phantom taps would pollute UI tests).

## Stability (run122-125)

- run122 (interactive, touch delivered): backboardd SEGV x2 at
  QuartzCore+0x30648 (NULL deref `ldr x0,[x0]` after call returns NULL),
  SB clean-exit x2 (relaunch cascade). backboardd/SB deaths: 0 in
  run119/120/121 -> touch delivery correlates with (probably enables)
  the render-path crash. Trigger TBD (app-driven CA commits).
- run123 (idle 30min): 0 backboardd/SB deaths. 3 light taps mid-run
  harmless. Touch events themselves are safe.
- logd death spiral: SIG10/SIG11 alternating in libsystem_platform
  memmove large-copy path (`ldnp`), x0==sp, same site every run
  (run122 slide 0x7df0c000: +0x7da4; run125 same site, different slide).
  Zero-byte .tracev3 files found on-device looked guilty, but wiping
  diagnostics+uuidtext (fresh store, run125) did NOT cure it: 4 deaths
  in 900s. Verdict: LIVE bug (message content/size?), not disk
  corruption. My first wipe attempt (surgery on pulled image + reflash)
  broke boot (launchd instant SEGV, run124) — cause unknown (flash
  integrity? macOS RW mount?); recovered by reflashing pristine fixA.
- run125 (fresh fixA, 900s): launchd healthy, backboardd 0 deaths, SB
  SIGBUS x1 at 0x1010107c8 (no slide consensus; needs follow-up to see
  if it recurs or was first-boot-only).
- Tap loop set to single pass (run123+).

# USB console and shell for the running iOS (plan, 2026-11-13)

Goal: debug iOS while it runs instead of reading a ramoops log after the deadline. Three stages, each useful alone:

1. **Live kernel log over USB**: the phone appears on the Mac as a USB serial port (CDC-ACM,
   `/dev/tty.usbmodem*`); `screen /dev/tty.usbmodem* 115200` shows the kernel log live, nothing lost to the
   260 KB ramoops wrap.
2. **A root shell over the same port**: a launchd job runs a shell on our device node; type commands into the
   running iOS (ps, logs, files, launching apps, defaults).
3. **SSH** (later): the USB function becomes a network adapter (CDC-ECM/NCM), XNU's IP stack configures it,
   dropbear serves `ssh root@<phone>`, `scp` works.

Nothing in our kernel drives USB today; Apple's USB device stack (IOUSBDeviceFamily, AppleUSBDeviceMux) is not in
open-source XNU, so this is our own minimal driver.

## Hardware (msm8994, reference/kernel-oneplus-msm8994)

The MTP board (which the OnePlus 2 follows) enables `usb3` (DWC3) with `hsphy0` and `ssphy0`
(msm8994-mtp.dtsi ~979); the ChipIdea `usb_otg` at 0xf9a55000 is the other controller. **Confirm on the phone
first**: in TWRP, `adb shell ls -l /sys/class/udc/` (Linux names the active UDC, expected `f9200000.dwc3`) and
`adb shell cat /proc/interrupts | grep -i dwc3`.

| block | address | source |
|---|---|---|
| DWC3 core (synopsys,dwc3), 0xfc000 | PA 0xf9200000, IRQ SPI 131 | msm8994.dtsi ~1751/1795 |
| QSCRATCH (Qualcomm wrapper) | 0xf9200000 + 0xF8800 | dwc3-msm.c QSCRATCH_REG_OFFSET |
| HS PHY (qcom,usb-hsphy) | 0xf92f8800 (= QSCRATCH) + phy_csr 0xf9b3a000 | hsphy-init 0x00D187A7, vbus-valid-override, ext-vbus-id |
| SS PHY (QMP) | 0xf9b38000 | not needed: run high-speed only |
| GDSC USB30 | 0xfc401e84 | msm-gdsc.dtsi |
| GCC base | 0xfc400000 | |
| GCC USB30_MASTER_CBCR / SLEEP / MOCK_UTMI | +0x3C8 / +0x3CC / +0x3D0 | drivers/clk/qcom/clock-gcc-8994.c |
| GCC SYS_NOC_USB3_AXI_CBCR | +0x3FC | |
| GCC USB30_MASTER_CMD_RCGR / MOCK_UTMI_CMD_RCGR | +0x3D4 / +0x3E8 | (ftbl_usb30_master_clk_src) |
| GCC USB_30_BCR / USB2_HS_PHY_ONLY_BCR / USB3_PHY_BCR | +0x3C0 / +0x4B0 / +0x1400 | block resets |
| GCC USB2_HS_PHY_SLEEP_CBCR / USB_PHY_CFG_AHB2PHY_CBCR | +0x4AC / +0x1A84 | |
| RPM clocks | ln_bb_clk (ref), cxo_dwc3 | via op2_rpm (bsd/dev/op2_rpm.c) if needed |
| Regulators | pm8994 L24 (3.3 V), L6 (1.8 V), S2/S1 corners | likely left on by LK (it ran fastboot) |

Linux references: drivers/usb/dwc3/{core.c,gadget.c,ep0.c,dwc3-msm.c}, drivers/usb/phy/phy-msm-hsusb.c,
drivers/usb/gadget/{f_acm.c,u_serial.c}. Register names below are DWC3 databook names as in core.h.

## Key idea: LK leaves USB alive

We arrive by `fastboot boot`: LK's fastboot just used this controller (DWC3, high speed) to receive our image. Its
clocks, GDSC, regulators and PHY are very likely still on. So stage 1 starts with a **read-only probe**, then
re-initialises only the core, not the whole power/clock tree.

## Stage 1: steps

1. **Probe, read-only, one boot** (new `OP2USB` IOService or code in an existing early driver). Log:
   GDSC USB30 reg (power on/off bits), each CBCR's bit 31 (CLK_OFF) and bit 0 (enable). **Only if** GDSC is on and
   master + AXI clocks are running, read DWC3 GSNPSID (0xC120, expect 0x5533xxxx), GCTL (0xC110), DCTL (0xC704),
   DSTS (0xC70C), GUSB2PHYCFG (0xC200). Reading registers of a clock-gated block can hang the bus -> deadline reset
   (harmless, lands in TWRP), so check the clocks first.
2. **Power and clocks if off**: GDSC on (clear SW_COLLAPSE, poll PWR_ON), enable the CBCRs (bit 0, poll CLK_OFF
   clear), master RCG at 125 MHz per ftbl if not configured. No voltage changes (CLAUDE.md).
3. **HS PHY** (phy-msm-hsusb.c): QSCRATCH HS_PHY_CTRL: UTMI_OTG_VBUS_VALID + SW_SESSVLD (vbus-valid-override,
   the cable is attached), PARAMETER_OVERRIDE_X = 0x00D187A7; keep the SS PHY in reset/suspended
   (GUSB3PIPECTL SUSPHY), DCFG speed = high speed (0) so the SS PHY is never used.
4. **Core**: GCTL.CORESOFTRESET + GUSB2PHYCFG.PHYSOFTRST pulse, GCTL.PRTCAPDIR = device (2), DCTL.CSFTRST, wait;
   event buffer: one 4 KB physically contiguous buffer (IOMallocContiguous, cache maintenance like op2_emmc ADMA2)
   -> GEVNTADRLO/HI(0), GEVNTSIZ(0), GEVNTCOUNT(0); DEVTEN: reset, connect done, disconnect, ep events.
5. **Endpoints**: EP0 OUT/IN (DEPCMD SETEPCONFIG / SETEPXFERRSRCCFG / STARTCFG per gadget.c
   `dwc3_gadget_start_config`), TRB ring for EP0 setup (8-byte SETUP), then DCTL.RUN_STOP = 1 (the Mac sees a
   device). Handle events by polling GEVNTCOUNT from a kernel thread first (interrupts later, SPI 131).
6. **Enumeration** (ep0.c as reference): GET_DESCRIPTOR (device, config, string), SET_ADDRESS (DCFG.DEVADDR),
   SET_CONFIGURATION (configure EP1 bulk IN/OUT 512 B, EP2 interrupt IN for CDC notifications), CDC class requests
   SET_LINE_CODING / GET_LINE_CODING / SET_CONTROL_LINE_STATE (accept and echo). Descriptors: CDC-ACM
   (class 2/2/1 + data class 0x0A), VID/PID 0x1209/0x0001 (pid.codes test) or Linux gadget serial 0x0525/0xA4A7;
   macOS's AppleUSBCDCACM binds by class.
7. **Data**: a 64 KB ring fed by the kernel's printf/kprintf path (hook where ramoops is fed) drained by bulk-IN
   TRBs; bulk-OUT received bytes into an input ring (stage 2).

Verify: `ls /dev/tty.usbmodem*` on the Mac during the round, `screen /dev/tty.usbmodem* 115200`. Keep the
ramoops log as it is (it still covers panics and early boot).

## Stage 2: shell

- Device node: a BSD character device `/dev/op2usb` (cdevsw read/write/select on the two rings; a tty layer is
  optional: plain read/write is enough for a non-job-control shell). XNU's tty helpers (ttyopen/ttread) can
  come later for line editing.
- Shell: stock iOS has none (`/bin` has only df and ps). Build `dash` (BSD licence) and `toybox` (0BSD: ls, cat,
  ps, kill, mount, dmesg, ...) from source with the Xcode 10.3 iPhoneOS 12.4 SDK (plain clang, as op2bench),
  install via the rootfs pipeline (like tools/rootfs_games.py) into /usr/local/bin.
- Launch: a launchd plist (KeepAlive) running `/usr/local/bin/dash -i` with StandardInPath/StandardOutPath/
  StandardErrorPath = /dev/op2usb, as root. New files need a rootfs rebuild (tools/rootfs-journal.sh).
- Useful immediately: `ps`, `/usr/local/bin/toybox dmesg`, reading crash and syslog files, `launchctl` (stock
  iOS has /bin/launchctl? check; else build a tiny one against liblaunch), opening an app by bundle id (a small
  helper calling SBSLaunchApplicationWithIdentifier / LSApplicationWorkspace openApplicationWithBundleID:).

## Stage 3: SSH (later)

CDC-ECM/NCM function instead of (or with) ACM; an XNU `ifnet` driver over the bulk endpoints (ifnet_allocate,
ether input/output); static IP (e.g. 10.11.0.2) on the phone, the Mac's side gets a USB Ethernet adapter;
dropbear (MIT licence) built for iOS 12 with host keys generated on first run.

## Risks and rules

- Never write voltages; only clocks/GDSC/core registers. The deadline and TWRP recovery are unaffected.
- Reading a gated block's registers can hang: probe clocks first.
- While iOS runs, the cable carries our device instead of adb; adb returns when TWRP boots after the deadline.
- One stage per boot round when it can break the boot; add `op2usb=0` to disable.

## Order of work

0. In TWRP: confirm the UDC (`/sys/class/udc`).
1. Probe boot (read-only register dump).
2. Core init + enumeration until the Mac lists the device (System Information / `ioreg -p IOUSB`).
3. Kernel log streaming.
4. /dev/op2usb + dash/toybox + launchd job: the shell.

## Probe result (round fable-49, 2026-11-13): LK leaves USB alive

op2_snapshot.c `op2_usb_probe` (read-only, boot and +5 min):

    GDSC usb30 0x80000000 (on); CBCR master 2000cff1 axi 00000001 utmi 00000001 sleep 00000001 hsphy 80000000
      ahb2phy 20008001 ssaux 00000001 sspipe 80000000
    RCGR master cfg 00002101, utmi cfg 00000113; BCRs 0
    DWC3 GSNPSID 5533250a (2.50a) GCTL 00112000 (device) GSTS 3e800000 GUSB2PHYCFG 00002500 GUSB3PIPECTL 030c0002
    DCFG 0008080c (speed 4 = SS, address 1) DCTL 00f00000 (RUN_STOP 0) DEVTEN 47 DSTS 00d316ec DALEPENA f
    GEVNT0 0x0f9a6000 size 0x1000 count 0 (LK's buffer: replace before running)
    QSCRATCH ctrl 19064190 general_cfg 39 hs_phy_ctrl 1d3279b2 param_override 00d190e4 ss_phy_ctrl 71210102

So stage 1 needs no GDSC/clock/PHY/regulator work: own event buffer, DCTL.CSFTRST, DCFG speed HS (0) and address 0,
DEVTEN, EP0 start config + SETUP TRB, RUN_STOP, then enumeration (gadget.c/ep0.c) and CDC-ACM.

## Stage 1 WORKS (round fable-50, 2026-11-13 02:25)

bsd/dev/op2_usb.c (added to bsd/conf/files; started from op2_snapshot.c after op2_usb_probe; console bytes via
pexpert/arm/pe_serial.c `op2_usb_putc_hook` in op2_ramoops_td0): takeover (RUN_STOP off, CSFTRST, own event
buffer masked/polled, DCFG high speed, DEVTEN reset/connect/disconnect, EP0 STARTCFG + config, SETUP TRB, RUN),
EP0 state machine (GET_DESCRIPTOR device/config/string, SET_ADDRESS, SET_CONFIGURATION, GET_STATUS,
GET/SET_LINE_CODING, SET_CONTROL_LINE_STATE; else stall), CDC-ACM (EP1 bulk 512 both ways, EP2 interrupt),
console ring 64 KB drained to bulk IN by a 1 ms polling thread, bulk OUT into a 4 KB ring (for stage 2).

First boot: the Mac listed "OP2 USB console" (0525:a4a7) at high speed; macOS asked to allow the accessory (Apple
silicon accessory security: click Allow every new boot it asks); /dev/cu.usbmodem00011 streamed the live kernel log.
Watch: `screen /dev/cu.usbmodem* 115200`, or tools/usb-console-watch.sh (records logs/usb-console.log during a
round).

Next: stage 2, the shell (dash + toybox, /dev/op2usb character device on the in/out rings, launchd job).

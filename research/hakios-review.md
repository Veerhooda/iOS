# HakiOS (oosexgang) review — what transfers to the OnePlus 2 iOS 12 port

Source: https://github.com/orgs/oosexgang/repositories (7 repos, cloned 2026-11-03 to /tmp/hakios).
HakiOS runs XNU xnu-8796.x (iOS 17-era) on SM7325 (POCO X5 Pro) to a working
lock screen / home screen / apps, plus QEMU virt. Our port is XNU xnu-4903
(iOS 12) on MSM8994. Techniques transfer; code/offsets mostly do not.

## Highest value (in suggested order)

1. `hakios-xnu-kernel/userland/src/eagl.c` + `mkeagl.py` + `eagl_slots.txt`
   + `gl_osmesa.c` — **EAGL→Mesa shim** (`libhakios.gl.dylib`, injected into
   backboardd). Replaces the 6 EAGLContext methods CA's render server uses
   (`initWithAPI`, `API`, `setParameter:to:`, `getMacroContextPrivate` table
   forwarded to Mesa gl*, `texImageIOSurface`, `attachImage`), with a
   copy-based fallback path for software drivers (texture fill on IOSurface
   seed change, readback on glFlush/glFinish — pure GLES calls).
   This is the staged answer to our `Software renderer! Simulating crash!`:
   EAGL→OSMesa first (slow, unblocks UI logic), EAGL→freedreno later.
   Needs `eagl_slots.txt` re-derived for iOS 12's OpenGLES (disasm
   `getMacroContextPrivate`). gltrace* boot-args included for debugging.

2. `hakios-xnu-kernel/drivers/gpu/hakios.gpu.cpp` + `hakios.adreno.c` —
   **kernel GPU driver speaking Linux msm-DRM ioctl ABI** through an
   IOUserClient, so Mesa freedreno works with a libdrm shim
   (`tools/mesa/hakios-drm`). Buffers = wired kernel memory in the 3D
   core's SMMU context, write-combined, + an IOSurface-backed buffer
   extension. Template for an Adreno 430 driver; A4xx has no GMU, so the
   bring-up is simpler than their A6xx. Polling waits, no IRQ/recovery yet.

3. `hakios-xnu-kernel/userland/src/inject.c` + `inject_sb.c` (`libhakios.dylib`,
   via DYLD_INSERT_LIBRARIES in launchd jobs + kernel AMFI stand-in) —
   SpringBoard/backboardd workarounds to compare with ours:
   - fake "activated" (else Setup/Buddy loop when mobileactivationd is absent);
   - setup assistant never needed; sb_delay; JPEG/PNG snapshots (no ASTC);
   - software-renderer vibrancy fix; poster-switcher guard;
   - **containermanagerd: stub out user-manager "personas" queries**
     (20×1s retries stall every container client — matches our
     containermanagerd thread explosion!);
   - backboardd: skip 5s ambient-light-sensor wait; virtual HID touch from
     a /dev node (cleaner than our kernel tap script).
   Note their warning: launchd panics if a system job runs a non-Apple binary.

4. `hakios-xnu-kernel/userland/src/rwtouch*` (check `src/`) — userland
   scripted touch + notify-based remote control. Better UI automation than
   our kernel blind-tap loop; adopt after UI is up.

5. `docs/` (architecture, porting, drivers-config, boot-args) — process
   reference for driver-per-boot-arg config and bring-up order.

## Lower priority / not applicable

- `hakios-uefi` (Mu-Silicium + XNU loader): our shim+fastboot chain works;
  no reason to switch boot architectures.
- `hakios-metal` (Metal on A6xx): long-term; CA falls back to GLES, so UI
  does not need it. Revisit for performance after GLES path works.
- `hakios-mt6768` (MediaTek): wrong SoC vendor, skip.
- `hakios-sm7325` (device repo): useful only as a second example of driver
  config/layout.
- QEMU virt target: theirs runs there; ours is MSM8994-specific — porting
  to virt is a project of its own, not a shortcut.

## Gaps to verify before reusing

- XNU/userland version drift (iOS 17-era vs our iOS 12): EAGL slot tables,
  SpringBoard method names, launchd behaviors all need re-derivation.
- Adreno 430 (A4xx) vs Adreno 7xx (A6xx): freedreno covers both, but power/
  clock/SMMU bring-up sequences differ per generation.
- License/attribution: check LICENSE files before adapting code.

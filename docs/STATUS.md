# Project status (2026-11-06, loop-26 running)

## TL;DR

iOS 12.5.7 runs on the OnePlus 2: SpringBoard + backboardd alive from boot,
36K display presents / 10 surfaces / lit panel over 2h (loop-25), clean eMMC
(0 fails / 2.59M reads), no crashing daemons. Remaining: eyes confirmation of
lockscreen-vs-home, AssistiveTouch/Home, SMP, speed.

## What is proven (with run numbers)

1. **Boot to UI**: loop-25 (run172, 2h): presents=36,250, shown=10,
   panel_nz=2000/2000, SB pid41 + backboardd pid45 alive throughout.
2. **Touch**: S3320 verified by hand (run122: taps, swipes, keyboard);
   synthetic autotap + periodic HOME + swipe-up drive wake/unlock unattended.
3. **Display pipeline**: `op2fb_client_ui=1` + per-minute FB timeline sampler;
   hakios lesson applied (panel never sleeps; idle sleep + dim vetoed).
4. **Sleep policy**: hakios-style `gSleepDisabledFlag` gate (default on,
   `syssleep=1` restores); every run self-resets on `op2_deadline`.
5. **eMMC reliability**: SDHCI driver with CMD12 abort, DAT0 wait, per-op
   retry after abort (-2/-3/-4, 3x, reads + writes), STATS + FAILRING.
   Retry demonstrably saves ops (`retry_ok=1`, loop-25).
6. **Crash guards (hunks)**: hunkQC (QuartzCore commit NULL), hunkCV
   (CoreVideo NULL-dict → SB min38 SEGV, fixed: 0 SB deaths since).
7. **Daemon hygiene**: locationd/suggestd crash-loops were poisoned on-disk
   state (pristine reflash → 0 deaths); lskdd parked (×248 stormer);
   mobileassetd must stay *alive* (parking hangs SB: launchd holds the
   message, SB's sync XPC has no timeout).
8. **Symbolication pipeline**: per-run slide (`op2slide.py`, trap-anchor
   fallback) → `op2sym.py` → `disas_cache.py` → 1-slot guards.

## Failing / open issues

| Issue | State | Next step |
|---|---|---|
| Lock-vs-home unresolved from logs | loop-25 lit but ambiguous | Eyes on loop-26 (same image, full TL) |
| No Home button / AssistiveTouch | `data_ark` seed did not show UI | AXS prefs round 2 |
| Single core (`cpus=1`) | SMP parked after run134 hang | ACC/APCS bring-up + crumbs |
| Slow boot (30–80 min to UI) | 1 core + cs page checks | cs fast path; daemon trim |
| Widget/Spotlight slow loops (6–13×/2h) | Background noise, tolerated | Trim extension after UI solid |
| 20-min deadline kills boots | Documented; 7200 is the standard | Keep 2h windows for UI runs |
| eMMC PHY lottery (0–16 fails/run) | Absorbed by retry; `-1` fails fast | Revisit `-1` if it appears |

## Run-loop methodology (what makes this work)

- One variable per loop; every claim needs a log line (TL/STATS/rings/SNAP).
- Fresh-flash bisect for userdata suspects; revert-test before blaming kernel.
- `fastboot boot` (never flash boot); BCB cookie → always back to recovery.
- Phone must stay visible in fastboot/adb at all times; 260 KB ramoops ring
  is the only truth (gated logs: userlog, FB notifications, tap posts).

## Recent run history (Nov 5–6)

- run141/143: 2–3h autotap soaks, 1.4–22M eMMC reads, 0 fails, touch + UI.
- run149–151: black-screen regression → `op2fb_client_ui=1` restored.
- run151 (loop-11): surface 3; loops 12–20: 20-min "stuck" = slow boot cut
  short (proven by revert + timeline analysis).
- run165–167 (loops 18–20): trim2 bisect (locationd ×204 storm), hunkQC
  ruled load-bearing-neutral, fixA revert still slow (deadline, not image).
- run168 (loop-21): stale-image accident hit surface 3; lskdd ×248 found.
- run169 (loop-22): AUTOHOME/AUTOSWIPE + lskdd park → 7K presents, surf 9;
  SB SEGV min38 → hunkCV designed from disassembly.
- run170 (loop-23): hunkCV held (0 SB deaths); SB blocked in MobileAsset
  sync XPC (wedged daemon, 16 eMMC fails).
- run171 (loop-24): parking disproven (identical hang); all fails rc=-2.
- run172 (loop-25): retry hardening → 0 fails; **36K presents, surf 10, lit**.
- run173 (loop-26, live): reproduce + full transition timeline (spam gated).

## Roadmap (ordered)

1. Eyes validation + lock/home determination.
2. AssistiveTouch/Home navigation (AXS prefs).
3. Stability soak (8h+) on the winning image (trim8 + retry kernel).
4. SMP bring-up (cpu1 via ACC, then big cluster).
5. Speed (cs fast path, trim) and app-render hardening (next hunks as found).

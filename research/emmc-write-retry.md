# eMMC write-retry hardening (design; implement after loop-24)

## Problem
Writes are single-shot: `op2_emmc_strategy` calls `sdhc_write_single_block`
once (op2_emmc.c:830/832). Any error -> `wr_fail++` -> EIO to HFS.
Loop-23: 10 lost writes / 2h. Lost journal writes rot the FS over days
(future poison). Reads already retry data-phase errors 3x
(`op2_emmc_read_retry`, :734); writes need the mirror.

## Fix (mirror reads exactly)
- Add `op2_emmc_write_retry(lba, buf)`: call write; while rc is -3/-4
  (data-phase: buffer-ready / data-end) and tries < 3, re-issue.
  Each attempt re-runs DAT0 wait + CMD12 abort + host reset internally
  (same as reads). Do NOT retry -1 (DAT0 wedged) / -2 (response),
  mirroring read policy.
- Use it at both strategy call sites (:830 RMW second half, :832 full
  sector). The RMW read half (:817) already retries.
- Count saves in a new `op2_emmc_write_retries_ok`. STATS format change:
  check tools/postrun.sh parsing before extending the line; if strict,
  fold into existing `retry_ok` or log via rate-limited path.
- Failure semantics unchanged: final failure still returns EIO (FS owns
  resubmit; HFS journal sees fewer of them).

## Why safe
- Same lock context (strategy holds op2_emmc_lock across the op).
- Same recovery primitives (already exercised by reads 2M+/run).
- No new commands: CMD24 re-issue after CMD12 abort + reset is the
  documented SDHCI error recovery sequence.
- Idempotent: rewriting the same 512B sector N times is harmless.

## Validation
Next 2h run: expect wr_fail -> 0 (or sharply down) with wr_retry_ok > 0
on PHY-lottery boots; FS stays mountable across 3+ consecutive runs.

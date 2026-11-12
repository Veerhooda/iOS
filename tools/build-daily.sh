#!/usr/bin/env bash
# Build a boot image for normal (hands-on) use: no synthetic input, no
# blank-surface fallback, 2 h safety deadline. Output: out/xnu-boot.img
#   ./tools/build-daily.sh            # incremental kernel build + image
#   ./tools/build-daily.sh --no-xnu   # repackage only (cmdline change)
#   EXTRA="op2_autoinput=2" ./tools/build-daily.sh   # append boot-args
set -euo pipefail
cd "$(dirname "$0")/.."

CMDLINE="-v cpus=4 keepsyms=1 op2_deadline=${DEADLINE:-7200} rd=emmcroot0 emmcroot=userdata rootrw=1 emmcrw=1 cs_enforcement_disable=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 op2fb_client_ui=0 op2_autoinput=${AUTOIN:-1} op2fb_keepon=1 op2_userlog=0${EXTRA:+ $EXTRA}"
if [ ${#CMDLINE} -ge 250 ]; then
  echo "cmdline is ${#CMDLINE} bytes; XNU truncates at 255. Trim it." >&2; exit 1
fi

KERNEL=xnu/xnu-4903.270.47/BUILD/obj/DEVELOPMENT_ARM64_MSM8994/kernel.development.msm8994
if [ "${1:-}" != "--no-xnu" ]; then
  bash ./tools/build-xnu.sh
  # build-xnu.sh pipes through tee, so check the log for compiler errors
  if grep -qE "error:|\*\*\* \[" build.log; then
    echo "=== XNU build FAILED; first errors: ===" >&2
    grep -nE "error:" build.log | head -20 >&2
    exit 1
  fi
fi
[ -f "$KERNEL" ] || { echo "missing $KERNEL" >&2; exit 1; }

python3 tools/mkadt.py out/op2-hardware.adt
python3 tools/mkpayload.py "$KERNEL" out/op2-hardware.adt out/op2-hardware-payload.bin --cmdline "$CMDLINE"
python3 shim/build.py --payload out/op2-hardware-payload.bin --output out/xnu-boot.img
echo "Built out/xnu-boot.img (cmdline ${#CMDLINE} bytes):"
echo "  $CMDLINE"

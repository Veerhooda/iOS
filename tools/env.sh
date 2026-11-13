#!/usr/bin/env bash
# Shared host paths for the OnePlus 2 iOS port tooling.
#
# Source from any script (any depth under the repo):
#   source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"/../tools/env.sh
#   # from tools/games/: .../../../tools/env.sh
#
# Where to place things (defaults, all overridable via environment):
#   $HOME/xcode-10.3/Xcode.app ......... Xcode 10.3 (period toolchain + iPhoneOS 12.4 SDK)
#   <repo>/out/shell/dash-0.5.12 ....... extracted dash source
#   <repo>/out/shell/dropbear-2026.94 .. extracted dropbear source
#   <repo>/out/shell/toybox-0.8.12 ..... extracted toybox source
# The repo itself can live anywhere; scripts find its root by walking up.
[ -n "${OP2_ENV_LOADED:-}" ] && return 0
OP2_ENV_LOADED=1

_op2_here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$_op2_here" != "/" ] && [ ! -f "$_op2_here/tools/round.sh" ]; do
	_op2_here="$(dirname "$_op2_here")"
done
REPO_ROOT="${REPO_ROOT:-$_op2_here}"
unset _op2_here

# Xcode 10.3 location. Override: XCODE_ROOT=/path/to/Xcode.app/Contents/Developer
XCODE_ROOT="${XCODE_ROOT:-$HOME/xcode-10.3/Xcode.app/Contents/Developer}"
IPHONEOS_SDK="${IPHONEOS_SDK:-$XCODE_ROOT/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS12.4.sdk}"
MACOSX_SDK="${MACOSX_SDK:-$XCODE_ROOT/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.14.sdk}"
XNU_SRC="${XNU_SRC:-$REPO_ROOT/xnu/xnu-4903.270.47}"

# Fail with a placement hint when the period toolchain is required but absent.
op2_require_xcode() {
	[ -d "$XCODE_ROOT" ] || {
		echo "error: Xcode 10.3 not found at $XCODE_ROOT" >&2
		echo "  extract Xcode 10.3 to \$HOME/xcode-10.3 (so that" >&2
		echo "  \$HOME/xcode-10.3/Xcode.app/Contents/Developer exists)," >&2
		echo "  or export XCODE_ROOT=/path/to/Xcode.app/Contents/Developer" >&2
		return 1
	}
	[ -d "$IPHONEOS_SDK" ] || {
		echo "error: iPhoneOS 12.4 SDK not found at $IPHONEOS_SDK" >&2
		echo "  it ships inside Xcode 10.3 (see above)" >&2
		return 1
	}
}

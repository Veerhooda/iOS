#!/usr/bin/env bash
# G3: Mesa 25.0.7 cross-built for the phone (iOS 12, arm64): the freedreno Gallium driver (fd4 for the A430)
# talking to the kernel's OP2GPU user client through hakios's libdrm stand-in (tools/mesa/hakios-drm), and
# hakios's air_run (draw a quad with a Metal shader, read a pixel back). Adapted from
# reference/hakios-xnu-kernel/tools/mk-mesa.sh (MIT); built with this Mac's clang against Xcode 10.3's
# iPhoneOS 12.4 SDK, -miphoneos-version-min=12.0. Everything under out/mesa-ios.
#
#   bash tools/mesa-ios.sh        -> out/mesa-ios/out/air_run
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
HAKIOS=$ROOT/reference/hakios-xnu-kernel
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
source "$ROOT/tools/env.sh"
op2_require_xcode || exit 1
SDK="$IPHONEOS_SDK"
MACSDK=$(xcrun --sdk macosx --show-sdk-path)
B=$ROOT/out/mesa-ios
SRC=$B/mesa-$VERSION
mkdir -p "$B/out"
cd "$B"

# the same patches as the phone build of hakios: their tools/mesa/*.patch except turnip (no Vulkan here) and
# osmesa's ES profile (no OSMesa here), and ours (tools/mesa-op2)
HPATCH="$HAKIOS/tools/mesa/air-frontend.patch $HAKIOS/tools/mesa/air-ir3.patch $HAKIOS/tools/mesa/freedreno-darwin.patch $(ls "$HAKIOS"/tools/mesa/ir3-*.patch)"
PATCHES=$(cat $HPATCH "$HAKIOS"/tools/mesa/hakios-drm/* "$ROOT"/tools/mesa-op2/*.patch | shasum -a 256 | cut -d' ' -f1)
if [ ! -d "$SRC" ] || [ "$(cat "$SRC/.op2-patches" 2>/dev/null)" != "$PATCHES" ]; then
	if [ ! -f mesa-$VERSION.tar.xz ]; then
		cp "$ROOT/out/mesa-host/mesa-$VERSION.tar.xz" . 2>/dev/null ||
			curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	fi
	echo "$SHA256  mesa-$VERSION.tar.xz" | shasum -a 256 -c --quiet
	rm -rf "$SRC" build
	tar xJf mesa-$VERSION.tar.xz
	for p in $HPATCH; do echo "patch: $(basename "$p")"; patch -s --no-backup-if-mismatch -d "$SRC" -p1 < "$p"; done
	cp -r "$HAKIOS/tools/mesa/hakios-drm" "$SRC/src/hakios-drm"
	# our kernel's service
	sed -i '' 's/#define HAKIOS_GPU_SERVICE     "HakiosGPU"/#define HAKIOS_GPU_SERVICE     "OP2GPU"/' "$SRC/src/hakios-drm/hakios.drm.c"
	grep -q '"OP2GPU"' "$SRC/src/hakios-drm/hakios.drm.c"
	rm -rf "$SRC/src/compiler/air"
	cp -r "$HAKIOS/tools/mesa/air" "$SRC/src/compiler/air"
	for p in "$ROOT"/tools/mesa-op2/*.patch; do echo "patch (op2): $(basename "$p")"; patch -s --no-backup-if-mismatch -d "$SRC" -p1 < "$p"; done
	echo "$PATCHES" > "$SRC/.op2-patches"
fi
# G5: the Metal driver core (userland/op2mtl), copied in on every run so an edit there is a ninja rebuild
# (the meson targets are in tools/mesa-op2/op2mtl-meson.patch and exist only when these files do)
for f in op2mtl.c op2mtl.h op2mtl_test.c; do
	cmp -s "$ROOT/userland/op2mtl/$f" "$SRC/src/compiler/air/$f" || cp "$ROOT/userland/op2mtl/$f" "$SRC/src/compiler/air/$f"
done

# IOKit's user-client API: the iPhoneOS SDK has IOKit.tbd but no headers (tools/ios-iokit: the calls a user
# client needs, as Apple's IOKitLib.h declares them)
rm -rf iokit-include
ln -sfn "$ROOT/tools/ios-iokit" iokit-include

[ -x "$ROOT/out/mesa-host/venv/bin/meson" ] || { echo "run tools/mesa-host.sh first (it makes the venv and bison)" >&2; exit 1; }
export PATH=$ROOT/out/mesa-host/tools/bin:$ROOT/out/mesa-host/venv/bin:$PATH

TC=/usr/bin
FLAGS="'-target', 'arm64-apple-ios12.0', '-isysroot', '$SDK', '-idirafter', '$B/iokit-include'"
# Xcode 10 kept libc++'s headers in the toolchain, not the SDK: the ones that match iOS 12's libc++.dylib
LIBCXX=$XCODE_ROOT/Toolchains/XcodeDefault.xctoolchain/usr/include/c++/v1
CXXFLAGS_EXTRA="'-nostdinc++', '-isystem', '$LIBCXX'"
cat > cross.ini <<INI
[binaries]
c = ['$TC/clang', $FLAGS]
cpp = ['$TC/clang++', $FLAGS, $CXXFLAGS_EXTRA]
objc = ['$TC/clang', $FLAGS]
ar = '$TC/ar'
ranlib = '$TC/ranlib'
strip = '$TC/strip'
pkg-config = 'false'

[built-in options]
c_link_args = ['-framework', 'IOKit', '-framework', 'CoreFoundation', '-Wl,-dead_strip']
cpp_link_args = ['-framework', 'IOKit', '-framework', 'CoreFoundation', '-Wl,-dead_strip']

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI

[ -f build/build.ninja ] || meson setup build "$SRC" --cross-file cross.ini --wrap-mode=nofallback \
	--buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled \
	-Degl=disabled -Dgbm=disabled -Dllvm=disabled -Dopengl=true -Dgles2=enabled -Dgles1=disabled \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled \
	-Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log 2>&1 || { tail -30 meson-setup.log; exit 1; }

ninja -C build src/gallium/drivers/freedreno/air_run > ninja.log 2>&1 || { grep -m10 -B2 -A10 "error" ninja.log; exit 1; }
cp build/src/gallium/drivers/freedreno/air_run out/air_run
strip -x out/air_run
codesign -s - -i com.apple.tzinit --force out/air_run
echo "mesa-ios.sh: $B/out/air_run ($(stat -f %z out/air_run) bytes)"
# the phone's copy is rewritten in place (tools/op2hfs.py), which cannot grow a file: say when it would not fit
ONDEV=9414816
[ "$(stat -f %z out/air_run)" -le $ONDEV ] || echo "mesa-ios.sh: WARNING: bigger than the phone's /usr/local/bin/op2-air-run ($ONDEV bytes)"

# G5: op2mtl.dylib (-> /usr/local/lib/op2mtl.dylib, a 24 MiB slot: tools/rootfs_g5.py) and its test
ninja -C build src/gallium/drivers/freedreno/op2mtl.dylib src/gallium/drivers/freedreno/op2mtl_test \
	> ninja-op2mtl.log 2>&1 || { grep -m20 -B2 -A10 "error" ninja-op2mtl.log; exit 1; }
cp build/src/gallium/drivers/freedreno/op2mtl.dylib out/op2mtl.dylib
cp build/src/gallium/drivers/freedreno/op2mtl_test out/op2mtl_test
strip -x out/op2mtl.dylib out/op2mtl_test
codesign -s - --force out/op2mtl.dylib
codesign -s - -i com.apple.tzinit --force out/op2mtl_test
echo "mesa-ios.sh: $B/out/op2mtl.dylib ($(stat -f %z out/op2mtl.dylib) bytes), op2mtl_test ($(stat -f %z out/op2mtl_test) bytes)"
[ "$(stat -f %z out/op2mtl.dylib)" -le 25165824 ] || echo "mesa-ios.sh: WARNING: op2mtl.dylib is bigger than its 24 MiB slot"

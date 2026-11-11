#!/usr/bin/env bash
# G2: Mesa 25.0.7 for THIS Mac with hakios's AIR (Metal shader) front end in it, and ir3 for the Adreno.
# macOS version of reference/hakios-xnu-kernel/tools/air/mk-host.sh (MIT, Darwin on redwood / HakiOS authors):
# the pinned release is fetched and checked, patched with the front end's air-*.patch and ir3's fixes
# (ir3-*.patch), and src/compiler/air is a link to the front end in the hakios checkout.
#
# Out, in out/mesa-host/build:
#   src/compiler/air/air2nir                       the front end alone (AIR -> NIR)
#   src/gallium/drivers/freedreno/ir3_compiler     AIR -> NIR -> ir3 (any Adreno, -g/--gpu picks one)
#
# Nothing goes outside out/mesa-host: meson, ninja, mako, pyyaml live in its venv.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
HAKIOS=$ROOT/reference/hakios-xnu-kernel
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
B=$ROOT/out/mesa-host
SRC=$B/mesa-$VERSION
mkdir -p "$B"
cd "$B"

[ -d "$HAKIOS/tools/mesa/air" ] || { echo "no $HAKIOS/tools/mesa/air (sparse clone of hakios-xnu-kernel)" >&2; exit 1; }

# freedreno-darwin.patch: no libdrm on Darwin; freedreno's drm layer is built against the hakios-drm stand-in
# (ir3_compiler links it). The turnip/osmesa patches are for the phone build and are left out.
# tools/mesa-op2/*.patch: ours, on top of the front end (iOS 12's AIR), applied to a COPY of it so the hakios
# checkout stays as fetched.
PATCHES=$(cat "$HAKIOS"/tools/mesa/air*.patch "$HAKIOS"/tools/mesa/ir3-*.patch "$HAKIOS"/tools/mesa/freedreno-darwin.patch "$HAKIOS"/tools/mesa/hakios-drm/* "$ROOT"/tools/mesa-op2/*.patch 2>/dev/null | shasum -a 256 | cut -d' ' -f1)
if [ ! -d "$SRC" ] || [ "$(cat "$SRC/.air-patches" 2>/dev/null)" != "$PATCHES" ]; then
	if [ ! -f mesa-$VERSION.tar.xz ]; then
		mkdir -p dl
		curl -sL --fail -o dl/mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
		mv dl/mesa-$VERSION.tar.xz .
	fi
	echo "$SHA256  mesa-$VERSION.tar.xz" | shasum -a 256 -c --quiet
	rm -rf "$SRC" build
	tar xJf mesa-$VERSION.tar.xz
	for p in "$HAKIOS"/tools/mesa/air-*.patch "$HAKIOS"/tools/mesa/ir3-*.patch "$HAKIOS"/tools/mesa/freedreno-darwin.patch; do
		echo "patch: $(basename "$p")"
		patch -s --no-backup-if-mismatch -d "$SRC" -p1 < "$p"
	done
	cp -r "$HAKIOS/tools/mesa/hakios-drm" "$SRC/src/hakios-drm"
	rm -rf "$SRC/src/compiler/air"
	cp -r "$HAKIOS/tools/mesa/air" "$SRC/src/compiler/air"
	for p in "$ROOT"/tools/mesa-op2/*.patch; do
		[ -f "$p" ] || continue
		echo "patch (op2): $(basename "$p")"
		patch -s --no-backup-if-mismatch -d "$SRC" -p1 < "$p"
	done
	echo "$PATCHES" > "$SRC/.air-patches"
fi

if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install --upgrade pip
	venv/bin/pip -q install meson ninja mako pyyaml packaging
fi
# macOS bison is 2.3; Mesa's ir3 grammar needs >= 2.6 (%code): GNU bison 3.8.2, built here
if [ ! -x tools/bin/bison ]; then
	mkdir -p dl-bison bison-src
	[ -f dl-bison/bison-3.8.2.tar.xz ] || curl -sL --fail -o dl-bison/bison-3.8.2.tar.xz https://ftp.gnu.org/gnu/bison/bison-3.8.2.tar.xz
	echo "9bba0214ccf7f1079c5d59210045227bcf619519840ebfa80cd3849cff5a5bf2  dl-bison/bison-3.8.2.tar.xz" | shasum -a 256 -c --quiet
	tar xJf dl-bison/bison-3.8.2.tar.xz -C bison-src
	(cd bison-src/bison-3.8.2 && ./configure --prefix="$B/tools" --disable-nls >/dev/null && make -j8 >/dev/null && make install >/dev/null)
fi
export PATH=$B/tools/bin:$B/venv/bin:$PATH

[ -f build/build.ninja ] || meson setup build "$SRC" --buildtype=debugoptimized \
	-Dplatforms= -Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm \
	-Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled \
	-Dopengl=true -Dgles2=enabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= \
	-Dgallium-vdpau=disabled -Dgallium-va=disabled -Dgallium-xa=disabled \
	-Dlmsensors=disabled -Dbuild-tests=true -Dzstd=disabled -Dxmlconfig=disabled \
	-Dexpat=disabled ${MESON_EXTRA:-} > meson-setup.log 2>&1 || { tail -30 meson-setup.log; exit 1; }

ninja -C build src/compiler/air/air2nir src/gallium/drivers/freedreno/ir3_compiler > ninja.log 2>&1 || { grep -m20 -B2 -A8 "error" ninja.log; exit 1; }

echo "mesa-host.sh: $B/build/src/compiler/air/air2nir"
echo "mesa-host.sh: $B/build/src/gallium/drivers/freedreno/ir3_compiler"

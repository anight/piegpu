#!/bin/bash
#
# Build the host Mesa used by tools/glslc: the vc4 gallium driver (the
# VideoCore IV shader compiler) and its no-hardware DRM shim, with the
# piegpu dump hook (patches/mesa-vc4-dump.patch).
#
# Result: third_party/mesa-install (not in git).
#
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TAG=mesa-26.2.3
SRC=$ROOT/third_party/mesa
PREFIX=$ROOT/third_party/mesa-install
JOBS=$(( $(nproc) > 1 ? $(nproc) - 1 : 1 ))	# a core left free

if [ ! -d "$SRC" ]; then
	git clone --depth 1 --branch "$TAG" https://gitlab.freedesktop.org/mesa/mesa.git "$SRC"
	git -C "$SRC" apply "$ROOT/patches/mesa-vc4-dump.patch"
fi

cd "$SRC"
[ -d build ] || meson setup build -Dprefix="$PREFIX" -Dbuildtype=debugoptimized \
	-Dgallium-drivers=vc4 -Dvulkan-drivers= -Dplatforms= -Dglx=disabled \
	-Degl=enabled -Dgbm=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dllvm=disabled -Dtools=drm-shim -Dvalgrind=disabled -Dlibunwind=disabled \
	-Dzstd=disabled
ninja -C build -j"$JOBS" install

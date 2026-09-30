#!/bin/bash
#
# Build the host Mesa used by tools/glslc: the vc4 gallium driver (the
# VideoCore IV shader compiler) and its no-hardware DRM shim, with the
# piegpu dump hook (patches/mesa-vc4-dump.patch, applied here to the
# submodule third_party/mesa, pinned at mesa-26.2.3; the submodule's
# changes are ignored in git status: .gitmodules).
#
# Result: third_party/mesa-install (not in git).
#
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=$ROOT/third_party/mesa
PATCH=$ROOT/patches/mesa-vc4-dump.patch
PREFIX=$ROOT/third_party/mesa-install
JOBS=$(( $(nproc) > 1 ? $(nproc) - 1 : 1 ))	# a core left free

if [ ! -f "$SRC/meson.build" ]; then
	git -C "$ROOT" submodule update --init --depth 1 third_party/mesa
fi
# the patch, unless it's applied already
if git -C "$SRC" apply --check "$PATCH" 2>/dev/null; then
	git -C "$SRC" apply "$PATCH"
elif ! git -C "$SRC" apply --check -R "$PATCH" 2>/dev/null; then
	echo "build-mesa.sh: third_party/mesa has changes other than $PATCH" >&2
	exit 1
fi

cd "$SRC"
[ -d build ] || meson setup build -Dprefix="$PREFIX" -Dbuildtype=debugoptimized \
	-Dgallium-drivers=vc4 -Dvulkan-drivers= -Dplatforms= -Dglx=disabled \
	-Degl=enabled -Dgbm=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dllvm=disabled -Dtools=drm-shim -Dvalgrind=disabled -Dlibunwind=disabled \
	-Dzstd=disabled
ninja -C build -j"$JOBS" install

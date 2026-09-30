#!/bin/bash
#
# Build dEQP-GLES2 (the Khronos VK-GL-CTS) for pgl on the PC: the tests' GL
# calls go through pgl (hosts/pc) to the RPi over USB.
#
# Result: third_party/deqp-build/modules/gles2/deqp-gles2 (run it with
# tools/deqp/run-deqp.sh). Needs the host pgl build (hosts/pc/build).
#
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CTS=$ROOT/third_party/VK-GL-CTS
BUILD=$ROOT/third_party/deqp-build
JOBS=$(( $(nproc) > 1 ? $(nproc) - 1 : 1 ))	# a core left free
HOST=$ROOT/hosts/pc

# the submodule (pinned at 1d3e817), and the sources it fetches itself
if [ ! -f "$CTS/CMakeLists.txt" ]; then
	git -C "$ROOT" submodule update --init --depth 1 third_party/VK-GL-CTS
fi
if [ ! -d "$CTS/external/glslang/src" ]; then
	python3 "$CTS/external/fetch_sources.py"		# glslang, spirv-tools, ... (1.2 GB)
fi
ln -sfn "$ROOT/tools/deqp/target" "$CTS/targets/pgl"

# pgl
cmake -S "$HOST" -B "$HOST/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo > /dev/null
make -C "$HOST/build" -j"$JOBS" pgl

# gl* functions of pgl.h for the platform's loader
mkdir -p "$BUILD/generated"
python3 - "$ROOT/libpgpu/gles/pgl.h" > "$BUILD/generated/pglFunctions.inl" <<'PY'
import re, sys
names = re.findall(r'^\w[\w *]*?\b(gl[A-Z]\w*) \(', open(sys.argv[1]).read(), re.M)
for n in sorted(set(names)):
    print(f'\t{{"{n}", (glw::GenericFuncType) &{n}}},')
PY

cmake -S "$CTS" -B "$BUILD" -DDEQP_TARGET=pgl -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DPGL_DEQP_DIR="$ROOT/tools/deqp" \
	-DPGL_INCLUDE_DIRS="$ROOT/libpgpu;$ROOT/libpgpu/gles;$ROOT/protocol" \
	-DPGL_GENERATED_DIR="$BUILD/generated" \
	-DPGL_LIBRARY="$HOST/build/libpgl.a"
make -C "$BUILD" -j"$JOBS" deqp-gles2

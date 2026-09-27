#!/bin/bash
#
# Build dEQP-GLES2 (the Khronos VK-GL-CTS) for pgl on the PC: the tests' GL
# calls go through pgl (hosts/pc) to the Zero over USB.
#
# Result: third_party/deqp-build/modules/gles2/deqp-gles2 (run it with
# tools/deqp/run-deqp.sh). Needs the host pgl build (hosts/pc/build).
#
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CTS=$ROOT/third_party/VK-GL-CTS
BUILD=$ROOT/third_party/deqp-build
COMMIT=1d3e8178af79e52d5467b379fad58f221fbe3d40
HOST=$ROOT/hosts/pc

if [ ! -d "$CTS" ]; then
	git clone https://github.com/KhronosGroup/VK-GL-CTS.git "$CTS"
	git -C "$CTS" checkout "$COMMIT"
	python3 "$CTS/external/fetch_sources.py"		# glslang, spirv-tools, ... (1.2 GB)
fi
ln -sfn "$ROOT/tools/deqp/target" "$CTS/targets/pgl"

# pgl
cmake -S "$HOST" -B "$HOST/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo > /dev/null
make -C "$HOST/build" -j"$(nproc)" pgl

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
make -C "$BUILD" -j"$(nproc)" deqp-gles2

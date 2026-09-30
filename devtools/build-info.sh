#!/bin/sh
#
# build-info.sh OUT.h CIRCLEDIR: this build's version, commit, time and
# Circle (gpu/build_info.h), as defines in OUT.h, at every build of the gpu
# app (gpu/CMakeLists.txt). The version: VERSION's major.minor.patch and a
# build number, the next of this checkout's (devtools/next-build.sh), unless
# PGPU_BUILD is given (devtools/build-gpu.sh, make-firmware.sh: one number
# for all their boards).
#
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(git -C "$here" rev-parse --show-toplevel)
build=${PGPU_BUILD:-$("$here/next-build.sh")}
cat > "$1.tmp" <<EOT
/* build_info_values.h - written by devtools/build-info.sh at every build */
#define PGPU_FW_VERSION	"$(cat "$root/VERSION").$build"
#define PGPU_FW_GIT	"$(git -C "$root" describe --always --dirty 2>/dev/null || echo unknown)"
#define PGPU_BUILT	"$(date -u +%Y-%m-%dT%H:%M:%SZ)"
#define PGPU_CIRCLE	"$(git -C "$2" describe --tags --always 2>/dev/null || echo unknown)"
EOT
mv "$1.tmp" "$1"

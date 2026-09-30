#!/bin/sh
#
# next-build.sh: the next build number of this checkout, printed. The count
# is in build/build-number (not in git); every firmware build takes the next
# one (gpu/Makefile; devtools/build-gpu.sh takes one for all its boards). The
# checkout's root comes from git: the Zero 2 W builds in a copy under build/.
#
set -e
root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
mkdir -p "$root/build"
n=$(( $(cat "$root/build/build-number" 2>/dev/null || echo 0) + 1 ))
echo "$n" > "$root/build/build-number"
echo "$n"

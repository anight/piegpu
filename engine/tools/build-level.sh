#!/bin/bash
#
# Compile a level for the BSP engine with ericw-tools (qbsp, vis, light):
#
#   engine/tools/build-level.sh NAME     engine/levels/NAME.map -> NAME.bsp
#
# ERICW (default ~/tools/ericw-tools): the unpacked Linux release
# (github.com/ericwa/ericw-tools, 2.0.0-alpha11 here). The textures come from
# the .map's WAD (engine/levels/textures.wad, made by make_base.py).
#
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
LEVELS=$HERE/../levels
ERICW=${ERICW:-$HOME/tools/ericw-tools}
NAME=${1:?usage: build-level.sh NAME}

cd "$LEVELS"
"$ERICW/qbsp" -nopercent -wadpath "$LEVELS" "$NAME.map" "$NAME.bsp"
"$ERICW/vis" -nopercent "$NAME.bsp"
"$ERICW/light" -nopercent -extra "$NAME.bsp"
rm -f "$NAME.prt" "$NAME.pts" "$NAME.lin" "$NAME.log" "$NAME-"*.log
ls -l "$NAME.bsp"

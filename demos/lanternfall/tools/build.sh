#!/bin/bash
#
# Lanternfall's levels, made and compiled (ericw-tools: qbsp, vis, light; as
# engine/tools/build-level.sh):
#
#   demos/lanternfall/tools/build.sh [NAME ...]     all six, or those named
#
# lfkit.py first if the textures changed (the WAD and the palette).
#
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
LEVELS=$HERE/../levels
ERICW=${ERICW:-$HOME/tools/ericw-tools}
NAMES=${*:-docks library forest market stair peak}

cd "$LEVELS"
for NAME in $NAMES; do
	python3 "$HERE/make_$NAME.py"
	"$ERICW/qbsp" -nopercent -wadpath "$LEVELS" "$NAME.map" "$NAME.bsp" > "$NAME.log" 2>&1 || { tail -20 "$NAME.log"; exit 1; }
	grep -i -E "leak|warning" "$NAME.log" | head -8 || true
	"$ERICW/vis" -nopercent "$NAME.bsp" > /dev/null
	"$ERICW/light" -nopercent -extra "$NAME.bsp" > /dev/null
	rm -f "$NAME.prt" "$NAME.pts" "$NAME.lin" "$NAME.log" "$NAME-"*.log "$NAME".*.json
	ls -l "$NAME.bsp" | awk '{print $5, $9}'
done

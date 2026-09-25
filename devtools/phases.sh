#!/bin/bash
#
# Boot an app and photograph its test phases in ONE port session: for each
# phase N, wait for "PHASE N READY" in the log (the app times its phases, about
# 10 s each) and take a photo (devtools/logs/phase-N.jpg). The whole log goes
# to phases.log.
#
#   devtools/phases.sh <app-dir> <number-of-phases>
#
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
LOGDIR=$HERE/logs
APP=$1
PHASES=$2
PL=$LOGDIR/phases.log

"$HERE/run.sh" "$APP" 0

TTY=
for i in $(seq 1 300); do
	for d in /sys/class/tty/ttyACM*; do
		[ -e "$d" ] || continue
		u=$(readlink -f "$d/device/..")
		if [ "$(cat "$u/idVendor"):$(cat "$u/idProduct")" = 1209:0001 ] && [ -w "/dev/$(basename "$d")" ]; then
			TTY=/dev/$(basename "$d")
		fi
	done
	[ -n "$TTY" ] && break
	sleep 0.1
done
[ -n "$TTY" ] || { echo "[phases] app did not connect"; exit 1; }

: > "$PL"
stty -F "$TTY" raw -echo
exec 3<>"$TTY"
( cat <&3 | stdbuf -oL tr -d '\r' >> "$PL" ) &
READER=$!
printf '\n' >&3			# host is listening: app replays its boot log

for n in $(seq 1 "$PHASES"); do
	for i in $(seq 1 600); do grep -q "PHASE $n READY" "$PL" && break; sleep 0.1; done
	if ! grep -q "PHASE $n READY" "$PL"; then
		echo "[phases] phase $n: no READY"
		break
	fi
	if [ -n "${NOPHOTO:-}" ]; then
		echo "[phases] phase $n ready (no photo)"
		continue
	fi
	sleep 1.5
	"$HERE/run.sh" --photo >/dev/null 2>&1
	cp "$LOGDIR/last.jpg" "$LOGDIR/phase-$n.jpg"
	echo "[phases] phase $n photographed"
done

sleep 1
kill "$READER" 2>/dev/null || true
stty -F "$TTY" -hupcl 2>/dev/null || true
timeout 5 true 3>&- || true
exec 3>&-

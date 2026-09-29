#!/bin/bash
#
# Build a Circle app, boot it on the RPi over USB and capture its log. It boots
# the Zero's 32-bit build (kernel.img, devtools/config.txt: arm_64bit=0); a
# Zero 2 W starts over USB too, with its kernel8.img and arm_64bit=1 (the
# installer page, or rpiboot -d on such a folder).
#
#   devtools/run.sh <app-dir> [log-seconds]   build, boot, capture log (default 10 s, 0 = boot only)
#   devtools/run.sh --log [log-seconds]        only capture the log of the running app
#   devtools/run.sh --reboot                   only reboot the running app into USB boot
#   devtools/run.sh --photo                    only take a photo of the display
#   devtools/run.sh --shot                     only get a screenshot from the running gpu app
#                                              (devtools/logs/screenshot.png)
#
# The RPi is connected with its "USB" port (no SD card inserted), so its boot ROM
# waits for rpiboot. Apps must use CDevLink (devtools/devlink.h), which provides the
# log over USB serial, the reboot magic and a watchdog.
#
# CMDLINE sets the kernel command line (cmdline.txt), e.g. CMDLINE="output=panel"
# for the gpu app (docs/protocol.md 14: output=, panel=, hdmi_pixels=).
#
# A photo of the display is taken after booting (CAMERA=/dev/videoN, default
# /dev/video0 if present, CAMERA=none to disable; CAMERA_ROTATE=0|180, default 180).
#
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
CIRCLEBOOT=$ROOT/circle/boot
BOOTDIR=$HERE/usbboot
LOGDIR=$HERE/logs

MAGIC=piegpu-reboot		# DEVLINK_REBOOT_MAGIC
VIDPIDS="1d50:614d 1209:0001"	# the gpu app (devtools/pgpugadget.h), other apps (DEVLINK_USB_*)
BOOTWAIT=30			# seconds to wait for the board to appear

mkdir -p "$LOGDIR"

log () { echo "[run] $*" >&2; }

# Print the tty of the running app's USB serial gadget (empty if none)
find_tty ()
{
	local d usb
	for d in /sys/class/tty/ttyACM*; do
		[ -e "$d" ] || continue
		usb=$(readlink -f "$d/device/..")
		case " $VIDPIDS " in *" $(cat "$usb/idVendor" 2>/dev/null):$(cat "$usb/idProduct" 2>/dev/null) "*)
			echo "/dev/$(basename "$d")"
			return;;
		esac
	done
}

boot_rom_present ()
{
	lsusb -d 0a5c:2763 >/dev/null 2>&1 || lsusb -d 0a5c:2764 >/dev/null 2>&1
}

reboot_app ()
{
	local tty
	tty=$(find_tty)
	if [ -n "$tty" ]; then
		log "Sending reboot magic to $tty"
		stty -F "$tty" raw -echo
		printf '\n%s\n' "$MAGIC" > "$tty"
	elif ! boot_rom_present; then
		log "No running app found, waiting for the board (the watchdog reboots a hung app)"
	fi
}

capture_log ()
{
	local secs=$1 tty i
	for ((i = 0; i < BOOTWAIT * 10; i++)); do
		tty=$(find_tty)
		# udev sets the group permissions shortly after the node appears
		[ -n "$tty" ] && [ -r "$tty" ] && [ -w "$tty" ] && break
		tty=
		sleep 0.1
	done
	if [ -z "$tty" ]; then
		log "App did not connect over USB within $BOOTWAIT s"
		return 1
	fi

	log "Log from $tty (${secs} s) -> $LOGDIR/last.log"
	stty -F "$tty" raw -echo
	exec 3<>"$tty"
	printf '\n' >&3			# tells the app that the host is listening
	[ -n "${SEND:-}" ] && (sleep 1; printf '%s' "$SEND" >&3) &
	timeout "$secs" cat <&3 | stdbuf -oL tr -d '\r' | tee "$LOGDIR/last.log" || true
	exec 3>&-
}

take_photo ()
{
	local camera=${CAMERA:-/dev/video0} filter
	[ -c "$camera" ] || return 0
	case "${CAMERA_ROTATE:-180}" in
	180)	filter="hflip,vflip," ;;
	*)	filter= ;;
	esac
	log "Photo from $camera -> $LOGDIR/last.jpg"
	# skip the first frames until auto exposure has settled
	ffmpeg -loglevel fatal -f v4l2 -input_format mjpeg -video_size 1920x1080 -i "$camera" \
		-vf "select=gte(n\\,30),${filter}scale=1280:-1" -frames:v 1 -y "$LOGDIR/last.jpg" || true
}

case "${1:-}" in
--log)
	capture_log "${2:-10}"
	take_photo
	exit
	;;
--photo)
	take_photo
	exit
	;;
--shot)
	SEND=s capture_log 4 >/dev/null
	python3 "$HERE/screenshot.py" "$LOGDIR/last.log" "$LOGDIR/screenshot.png" --scale 2
	exit
	;;
--reboot)
	reboot_app
	exit
	;;
""|-h|--help)
	sed -n '3,15p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
	;;
esac

APP=$(cd "$1" && pwd)
SECS=${2:-10}

log "Building $APP"
make -C "$APP" -s -j"$(( $(nproc) > 1 ? $(nproc) - 1 : 1 ))"

mkdir -p "$BOOTDIR"
cp "$CIRCLEBOOT"/{bootcode.bin,start.elf,fixup.dat} "$BOOTDIR/"
cp "$HERE/config.txt" "$BOOTDIR/config.txt"
# the kernel command line (Circle's options and the app's, e.g. CMDLINE="output=hdmi")
printf '%s\n' "${CMDLINE:-}" > "$BOOTDIR/cmdline.txt"
cp "$APP/kernel.img" "$BOOTDIR/kernel.img"

reboot_app

log "Serving boot files with rpiboot"
timeout $((BOOTWAIT + 30)) rpiboot -d "$BOOTDIR" >"$LOGDIR/rpiboot.log" 2>&1 || {
	log "rpiboot failed, see $LOGDIR/rpiboot.log"
	tail -5 "$LOGDIR/rpiboot.log" >&2
	exit 1
}

if [ "$SECS" = 0 ]; then		# boot only, the caller talks to the app itself
	exit 0
fi

capture_log "$SECS"
take_photo

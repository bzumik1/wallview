#!/bin/sh
# Usage: tools/touchfw/panel-cstfw.sh root@<panel> info|boot-check|boot-checksum
#        tools/touchfw/panel-cstfw.sh root@<panel> flash <image> <current-checksum> --erase-and-program
#
# Builds cstfw in the Lima VM `wallview`, takes the panel lock, unbinds the touch
# driver, runs cstfw on the panel and binds the driver again. Touch is gone for a
# few seconds (about 12 s for flash). Only flash writes the controller's flash.
#
# flash runs detached on the panel (output in /tmp/cstfw.log), so a dropped SSH
# connection or Ctrl-C on the Mac cannot stop it between pages. After a failed
# flash the driver is NOT bound again (its probe would reset the chip into a
# half-written application): run the same flash again, as cstfw says.
set -eu
PANEL=${1:-}
CMD=${2:-}
usage() {
	echo "usage: $0 root@<panel> info|boot-check|boot-checksum" >&2
	echo "       $0 root@<panel> flash <image> <current-checksum> --erase-and-program" >&2
	exit 2
}
[ -n "$PANEL" ] || usage
case "$CMD" in
info | boot-check | boot-checksum) [ $# -eq 2 ] || usage ;;
flash)
	[ $# -eq 5 ] && [ -f "$3" ] && [ "$5" = --erase-and-program ] || usage
	case "$4" in 0x[0-9a-fA-F]*) ;; *) echo "current checksum must be hex, e.g. 0x99630e41" >&2; exit 2 ;; esac
	;;
*) usage ;;
esac

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=$REPO/out/cstfw/cstfw
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
# The repository is mounted at the same path in the VM; ~ is expanded inside the VM.
limactl shell wallview -- bash -c '~/build/br/host/bin/aarch64-none-linux-gnu-gcc -O2 -Wall -Wextra -Werror -o "$1" "$2"' \
	_ "$OUT" "$HERE/cstfw.c"
[ -s "$OUT" ] || { echo "cstfw was not built" >&2; exit 1; }

OWNER="${WALLVIEW_AGENT:-panel-cstfw} · touch controller $CMD"
"$REPO/tools/panel-lock.sh" "$PANEL" acquire "$OWNER"
trap '"$REPO/tools/panel-lock.sh" "$PANEL" release "$OWNER"' EXIT

ssh "$PANEL" 'cat > /tmp/cstfw && chmod +x /tmp/cstfw' < "$OUT"

if [ "$CMD" != flash ]; then
	# The remote trap binds the driver again whatever happens, also on a dropped connection.
	ssh "$PANEL" "D=/sys/bus/i2c/drivers/Hynitron-TS
		trap 'echo 1-005a > \$D/bind 2>/dev/null' EXIT
		trap 'exit 1' HUP INT TERM
		echo 1-005a > \$D/unbind
		/tmp/cstfw $CMD" && rc=0 || rc=$?
else
	ssh "$PANEL" 'cat > /tmp/cst-image.bin' < "$3"
	ssh "$PANEL" "D=/sys/bus/i2c/drivers/Hynitron-TS
		command -v setsid > /dev/null || { echo 'no setsid on the panel' >&2; exit 3; }
		echo 1-005a > \$D/unbind
		rm -f /tmp/cstfw.log /tmp/cstfw.rc
		setsid sh -c '/tmp/cstfw flash /tmp/cst-image.bin --current-checksum $4 --erase-and-program > /tmp/cstfw.log 2>&1; echo \$? > /tmp/cstfw.rc
			if [ \"\$(cat /tmp/cstfw.rc)\" = 0 ] || [ \"\$(cat /tmp/cstfw.rc)\" = 2 ]; then echo 1-005a > /sys/bus/i2c/drivers/Hynitron-TS/bind; fi
			rm -f /tmp/cst-image.bin' < /dev/null > /dev/null 2>&1 &"
	# Follow the log until the detached run has finished; losing this connection does not stop it.
	ssh "$PANEL" 'while [ ! -f /tmp/cstfw.rc ]; do sleep 1; done; cat /tmp/cstfw.log; exit $(cat /tmp/cstfw.rc)' && rc=0 || rc=$?
	if [ "$rc" = 1 ]; then
		echo "FLASH FAILED: driver left unbound on purpose. Do not reboot or power off; run the same flash again." >&2
		exit 1
	fi
fi

sleep 3
ssh "$PANEL" 'ls /sys/bus/i2c/drivers/Hynitron-TS/1-005a > /dev/null 2>&1 && echo "driver bound again" || { echo "DRIVER NOT BOUND" >&2; exit 1; }
	pidof wallview-touch > /dev/null && echo "wallview-touch running" || echo "wallview-touch not running (resampling off?)"
	dmesg | tail -5'
exit "$rc"

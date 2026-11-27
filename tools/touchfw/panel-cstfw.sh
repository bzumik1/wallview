#!/bin/sh
# Usage: tools/touchfw/panel-cstfw.sh root@<panel> info|boot-check
#        tools/touchfw/panel-cstfw.sh root@<panel> flash <image> --erase-and-program
# Cross-compiles cstfw in the Lima VM, copies it to the panel, unbinds the touch
# driver, runs it and binds the driver again. Touch is gone for a few seconds
# (about 12 s for flash). info and boot-check never write the controller's flash.
set -e
PANEL=$1
CMD=${2:-info}
[ -n "$PANEL" ] || { echo "usage: $0 root@<panel> info|boot-check|flash <image> --erase-and-program" >&2; exit 2; }
if [ "$CMD" = flash ]; then
	[ -f "$3" ] && [ "$4" = --erase-and-program ] || { echo "usage: $0 root@<panel> flash <image> --erase-and-program" >&2; exit 2; }
	ssh "$PANEL" 'cat > /tmp/cst-image.bin' < "$3"
	CMD="flash /tmp/cst-image.bin --erase-and-program"
fi
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/cstfw
limactl shell wallview -- ~/build/br/host/bin/aarch64-none-linux-gnu-gcc -O2 -Wall -Wextra -Werror \
	-o "$OUT" "$HERE/cstfw.c"
ssh "$PANEL" 'cat > /tmp/cstfw && chmod +x /tmp/cstfw' < "$OUT"
ssh "$PANEL" "D=/sys/bus/i2c/drivers/Hynitron-TS
	echo 1-005a > \$D/unbind
	/tmp/cstfw $CMD; rc=\$?
	echo 1-005a > \$D/bind
	sleep 1
	ls \$D/1-005a >/dev/null && echo 'driver bound again' || echo 'DRIVER NOT BOUND'
	dmesg | tail -5
	exit \$rc"

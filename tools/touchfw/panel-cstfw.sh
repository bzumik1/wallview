#!/bin/sh
set -eu
PANEL=${1:-}
CMD=${2:-}
SSH="ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=3"
usage() {
	sed -n '2,5p' "$0" | sed 's/^# //' >&2
	exit 2
}
[ -n "$PANEL" ] || usage
case "$CMD" in
info | boot-check | boot-checksum | follow) [ $# -eq 2 ] || usage ;;
flash)
	[ $# -eq 5 ] && [ -f "$3" ] && [ "$5" = --erase-and-program ] || usage
	case "$4" in
	0x | 0x*[!0-9a-fA-F]* | 0x?????????*) echo "current checksum must be 0x and 1-8 hex digits" >&2; exit 2 ;;
	0x*) ;;
	*) echo "current checksum must be 0x and 1-8 hex digits" >&2; exit 2 ;;
	esac
	;;
recover) [ $# -eq 4 ] && [ -f "$3" ] && [ "$4" = --erase-and-program ] || usage ;;
*) usage ;;
esac

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OWNER="${WALLVIEW_AGENT:-panel-cstfw $(hostname -s 2>/dev/null || echo mac)} · touch controller"
OWNER=$(printf '%s' "$OWNER" | tr -d "'")

MARK=/data/wallview/cstfw-failed
RUNNER_OWNER="touch controller flash (cstfw-run)"

follow() {
	$SSH "$PANEL" 'R=/tmp/cstfw.run; n=0
		[ -d $R ] || { echo "no flash run on the panel" >&2; exit 96; }
		while [ ! -s $R/rc ]; do
			n=$((n + 1))
			if [ $n -gt 300 ]; then echo "no result after 300 s" >&2; exit 97; fi
			if [ $n -gt 5 ] && ! pidof cstfw > /dev/null && { [ ! -s $R/pid ] || ! kill -0 "$(cat $R/pid)" 2>/dev/null; } && [ ! -s $R/rc ]; then
				echo "the flash runner ended without a result" >&2; cat $R/log 2>/dev/null; exit 98
			fi
			sleep 1
		done
		cat $R/log; exit "$(cat $R/rc)"' && rc=0 || rc=$?
	case "$rc" in
	0) echo "flash done and verified" ;;
	2 | 3 | 5) echo "flash refused or boot mode not reached, nothing written (rc $rc)" >&2 ;;
	96) ;;
	97) echo "no result yet: the flash may still run on the panel. Run: $0 $PANEL follow" >&2 ;;
	255) echo "connection lost; the flash continues on the panel and keeps the lock. Run: $0 $PANEL follow" >&2 ;;
	*)
		echo "FLASH FAILED or unfinished (rc $rc): $MARK is set, driver unbound, agent paused, lock held." >&2
		echo "Run: $0 $PANEL recover <vendor image> --erase-and-program (a reboot is not needed; after one, recover works the same)" >&2
		;;
	esac
	return "$rc"
}

if [ "$CMD" = follow ]; then
	follow && exit 0 || exit $?
fi

OUT=$REPO/out/cstfw/cstfw
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
limactl shell wallview -- bash -c '~/build/br/host/bin/aarch64-none-linux-gnu-gcc -O2 -Wall -Wextra -Werror -o "$1" "$2"' \
	_ "$OUT" "$HERE/cstfw.c"
[ -s "$OUT" ] || { echo "cstfw was not built" >&2; exit 1; }

$SSH "$PANEL" 'R=/tmp/cstfw.run
	[ -d $R ] || exit 0
	if [ ! -s $R/rc ] && { pidof cstfw > /dev/null || { [ -s $R/pid ] && kill -0 "$(cat $R/pid)" 2>/dev/null; }; }; then
		echo "a flash is running on the panel: use follow" >&2; exit 75
	fi
	echo "removing the finished run directory (rc $(cat $R/rc 2>/dev/null || echo none))"
	rm -rf $R'

if [ "$CMD" = recover ] && $SSH "$PANEL" "[ -e $MARK ] && [ \"\$(cat /tmp/wallview-panel.lock/owner 2>/dev/null)\" = '$RUNNER_OWNER' ]"; then
	echo "taking over the lock kept by the failed flash: $($SSH "$PANEL" "cat $MARK")"
else
	"$REPO/tools/panel-lock.sh" "$PANEL" acquire "$OWNER"
fi
trap '"$REPO/tools/panel-lock.sh" "$PANEL" release "$OWNER" 2>/dev/null || true' EXIT

$SSH "$PANEL" 'cat > /tmp/cstfw && chmod +x /tmp/cstfw' < "$OUT"

case "$CMD" in
info | boot-check | boot-checksum)
	$SSH "$PANEL" "D=/sys/bus/i2c/drivers/Hynitron-TS
		bound=0
		[ -e \$D/1-005a ] && bound=1
		if [ -e $MARK ]; then echo 'a flash failed earlier ($MARK): the driver stays unbound' >&2; bound=0; fi
		trap '[ \$bound = 1 ] && echo 1-005a > \$D/bind 2>/dev/null' EXIT
		trap 'exit 1' HUP INT TERM
		if [ \$bound = 1 ]; then echo 1-005a > \$D/unbind; fi
		/tmp/cstfw $CMD" && rc=0 || rc=$?
	;;
flash | recover)
	ARG=recover
	[ "$CMD" = flash ] && ARG=$4
	$SSH "$PANEL" 'cat > /tmp/cstfw-run.sh' < "$HERE/cstfw-run.sh"
	$SSH "$PANEL" 'cat > /tmp/cst-image.bin' < "$3"
	$SSH "$PANEL" "command -v setsid > /dev/null || { echo 'no setsid on the panel' >&2; exit 3; }
		mkdir /tmp/cstfw.run || exit 75
		printf '%s\n' '$RUNNER_OWNER' > /tmp/wallview-panel.lock/owner
		setsid sh /tmp/cstfw-run.sh '$ARG' < /dev/null > /dev/null 2>&1 &"
	follow && rc=0 || rc=$?
	[ "$rc" = 0 ] || exit "$rc"
	;;
esac

if $SSH "$PANEL" "[ -e $MARK ]"; then
	echo "a flash failed earlier: driver intentionally unbound" >&2
	exit "${rc:-1}"
fi
sleep 3
$SSH "$PANEL" 'ls /sys/bus/i2c/drivers/Hynitron-TS/1-005a > /dev/null 2>&1 && echo "driver bound" || { echo "DRIVER NOT BOUND" >&2; exit 1; }
	pidof wallview-touch > /dev/null && echo "wallview-touch running" || echo "wallview-touch not running (resampling off?)"
	dmesg | tail -5'
exit "$rc"

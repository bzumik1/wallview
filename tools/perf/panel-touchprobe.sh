#!/bin/sh
set -eu
PANEL=${1:-}
SECONDS_RUN=${2:-}
NAME=${3:-}
COUNTER=${4:-}
[ -n "$PANEL" ] && [ -n "$NAME" ] && case "$SECONDS_RUN" in '' | *[!0-9]*) false ;; *) [ "$SECONDS_RUN" -ge 1 ] && [ "$SECONDS_RUN" -le 120 ] ;; esac || {
	echo "usage: $0 root@<panel> <seconds 1-120> <name> [counter<ms>]" >&2
	exit 2
}
case "$NAME" in *[!A-Za-z0-9._-]*) echo "name: letters, digits, . _ - only" >&2; exit 2 ;; esac
case "$COUNTER" in "" | counter[5-9][0-9] | counter[1-9][0-9][0-9] | counter[1-9][0-9][0-9][0-9]) ;; *) echo "counter: counter<50-9999 ms>" >&2; exit 2 ;; esac
EXTRA=
[ -z "$COUNTER" ] || EXTRA="level ack $COUNTER"
SSH="ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=3"
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OWNER=$(printf '%s' "${WALLVIEW_AGENT:-touchprobe $(hostname -s 2>/dev/null || echo mac)} · touchprobe · $$.$(date +%s)" | tr -d "'")
MARK=/data/wallview/cstfw-failed
OUTDIR=$REPO/local/touchprobe
mkdir -p "$OUTDIR" "$REPO/out/touchprobe"
LOCAL=$OUTDIR/$NAME.txt
[ ! -e "$LOCAL" ] || { echo "$LOCAL exists" >&2; exit 2; }

locked=0
BIN=
cleanup() {
	[ -n "$BIN" ] && rm -f "$BIN"
	if [ "$locked" = 1 ]; then
		"$REPO/tools/panel-lock.sh" "$PANEL" release "$OWNER" 2>/dev/null || true
	fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

BIN=$(mktemp "$REPO/out/touchprobe/touchprobe.XXXXXX")
limactl shell wallview -- bash -c '~/build/br/host/bin/aarch64-none-linux-gnu-gcc -O2 -Wall -Wextra -o "$1" "$2"' _ "$BIN" "$HERE/touchprobe.c"
[ -s "$BIN" ] || { echo "touchprobe was not built" >&2; exit 1; }

locked=1
"$REPO/tools/panel-lock.sh" "$PANEL" acquire "$OWNER"

$SSH "$PANEL" "[ ! -e $MARK ] && [ ! -d /tmp/cstfw.run ]" || { echo "failed-flash marker or flash run present: not probing" >&2; exit 2; }
$SSH "$PANEL" 'cat > /tmp/touchprobe.q && chmod +x /tmp/touchprobe.q' < "$BIN"
$SSH "$PANEL" "D=/sys/bus/i2c/drivers/Hynitron-TS
	[ -e \$D/1-005a ] || { echo 'driver not bound: not probing' >&2; exit 2; }
	trap 'echo 1-005a > \$D/bind 2>/dev/null; rm -f /tmp/touchprobe.q' EXIT
	trap 'exit 1' HUP INT TERM
	echo 1-005a > \$D/unbind
	/tmp/touchprobe.q /dev/i2c-1 /dev/gpiochip0 13 $SECONDS_RUN $EXTRA" > "$LOCAL" && rc=0 || rc=$?
sleep 3
$SSH "$PANEL" 'ls /sys/bus/i2c/drivers/Hynitron-TS/1-005a > /dev/null 2>&1 && echo "driver bound" || { echo "DRIVER NOT BOUND" >&2; exit 1; }
	pidof wallview-touch > /dev/null && echo "wallview-touch running" || echo "wallview-touch not running"'
python3 "$HERE/touchprobe.py" "$LOCAL"
exit "$rc"

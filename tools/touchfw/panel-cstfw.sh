#!/bin/sh
set -eu
PANEL=${1:-}
CMD=${2:-}
SSH="ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=3"
usage() {
	cat >&2 <<EOF
usage: $0 root@<panel> info|boot-check|boot-checksum
       $0 root@<panel> flash <image> <current-checksum> --erase-and-program
       $0 root@<panel> recover <vendor image> --erase-and-program
       $0 root@<panel> follow|state
EOF
	exit 2
}
[ -n "$PANEL" ] || usage
case "$CMD" in
info | boot-check | boot-checksum | follow | state) [ $# -eq 2 ] || usage ;;
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
LOCK=/tmp/wallview-panel.lock
R=/tmp/cstfw.run
RUNNER_OWNER="touch controller flash (cstfw-run)"
LIVE='live() { [ -s '$R'/rc ] && return 1; pidof cstfw > /dev/null && return 0; if [ -s '$R'/pid ]; then kill -0 "$(cat '$R'/pid)" 2>/dev/null; return; fi; [ -s '$R'/started ] || return 0; [ $(($(date +%s) - $(cat '$R'/started))) -lt 120 ]; }'

state() {
	$SSH "$PANEL" "if [ -e $MARK ]; then echo \"failed-flash marker: \$(cat $MARK)\"; else echo 'failed-flash marker: none'; fi
		if [ -d $LOCK ]; then echo \"panel lock: \$(cat $LOCK/owner 2>/dev/null) since \$(cat $LOCK/since 2>/dev/null)\"; else echo 'panel lock: free'; fi
		if [ -e /sys/bus/i2c/drivers/Hynitron-TS/1-005a ]; then echo 'touch driver: bound'; else echo 'touch driver: not bound'; fi
		if [ -s /tmp/cstfw.agent-pids ]; then echo \"agent paused: \$(cat /tmp/cstfw.agent-pids)\"; else echo 'agent paused: no'; fi
		if [ -d $R ]; then echo \"flash run: rc \$(cat $R/rc 2>/dev/null || echo none)\"; else echo 'flash run: none'; fi
		[ ! -e $MARK ]" && st=0 || st=$?
	case "$st" in
	1) echo "next: $0 $PANEL recover <vendor image> --erase-and-program" >&2 ;;
	255) echo "could not read the panel state (ssh failed)" >&2 ;;
	esac
	return "$st"
}

follow() {
	$SSH "$PANEL" "R=$R; n=0
		$LIVE
		[ -d \$R ] || { echo 'no flash run on the panel' >&2; exit 96; }
		while [ ! -s \$R/rc ]; do
			n=\$((n + 1))
			if [ \$n -gt 300 ]; then echo 'no result after 300 s' >&2; exit 97; fi
			if [ \$n -gt 5 ] && ! live && [ ! -s \$R/rc ]; then
				echo 'the flash runner ended without a result' >&2; cat \$R/log 2>/dev/null; exit 98
			fi
			sleep 1
		done
		cat \$R/log; exit \"\$(cat \$R/rc)\"" && rc=0 || rc=$?
	case "$rc" in
	0) echo "flash done and verified" ;;
	2 | 3 | 5) echo "flash refused or boot mode not reached, nothing written in this run (rc $rc)" >&2 ;;
	96) ;;
	97 | 255)
		echo "no result yet or connection lost: the flash may still run on the panel. Run: $0 $PANEL follow" >&2
		return "$rc"
		;;
	*) echo "FLASH FAILED, unfinished or runner lost (rc $rc)" >&2 ;;
	esac
	state || true
	return "$rc"
}

if [ "$CMD" = follow ]; then
	follow && exit 0 || exit $?
fi
if [ "$CMD" = state ]; then
	state && exit 0 || exit $?
fi

locked=0
OUT=
cleanup() {
	[ -n "$OUT" ] && rm -f "$OUT"
	if [ "$locked" = 1 ]; then
		"$REPO/tools/panel-lock.sh" "$PANEL" release "$OWNER" 2>/dev/null || true
	fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

mkdir -p "$REPO/out/cstfw"
OUT=$(mktemp "$REPO/out/cstfw/cstfw.XXXXXX")
limactl shell wallview -- bash -c '~/build/br/host/bin/aarch64-none-linux-gnu-gcc -O2 -Wall -Wextra -Werror -o "$1" "$2"' \
	_ "$OUT" "$HERE/cstfw.c"
[ -s "$OUT" ] || { echo "cstfw was not built" >&2; exit 1; }

$SSH "$PANEL" "R=$R; $LIVE; if [ -d \$R ] && live; then echo 'a flash run is active on the panel: use follow' >&2; exit 75; fi"

took_over=0
tk=0
if [ "$CMD" = recover ]; then
	$SSH "$PANEL" "R=$R; $LIVE
		[ \"\$(cat $LOCK/owner 2>/dev/null)\" = '$RUNNER_OWNER' ] || exit 1
		if [ -d \$R ] && live; then exit 1; fi
		date '+%Y-%m-%dT%H:%M:%S%z' > $LOCK/since" && took_over=1 || tk=$?
	if [ "$took_over" = 1 ]; then
		echo "taking over the lock kept by an earlier flash run"
	elif [ "$tk" = 255 ]; then
		echo "ssh failed while checking the lock" >&2
		exit 255
	fi
fi
if [ "$took_over" = 0 ]; then
	locked=1
	"$REPO/tools/panel-lock.sh" "$PANEL" acquire "$OWNER" || { st=$?; locked=0; exit "$st"; }
fi

$SSH "$PANEL" "R=$R; $LIVE
	[ -d \$R ] || exit 0
	if live; then echo 'a flash run became active: stopping' >&2; exit 75; fi
	mkdir -p /data/wallview
	cp \$R/log /data/wallview/cstfw-last.log 2>/dev/null
	echo \"removing the finished run directory (rc \$(cat \$R/rc 2>/dev/null || echo none), log kept in /data/wallview/cstfw-last.log)\"
	rm -rf \$R"

put() {
	$SSH "$PANEL" "cat > '$2'" < "$1"
	got=$($SSH "$PANEL" "sha256sum '$2'" | cut -d' ' -f1)
	[ -n "$got" ] && [ "$got" = "$(shasum -a 256 "$1" | cut -d' ' -f1)" ] || { echo "upload of $1 to $2 not verified" >&2; return 1; }
}

$SSH "$PANEL" "command -v sha256sum > /dev/null" || { echo "sha256sum missing on the panel" >&2; exit 3; }

case "$CMD" in
info | boot-check | boot-checksum)
	Q=/tmp/cstfw.q
	put "$OUT" "$Q"
	$SSH "$PANEL" "chmod +x $Q
		D=/sys/bus/i2c/drivers/Hynitron-TS
		bound=0
		[ -e \$D/1-005a ] && bound=1
		if [ -e $MARK ]; then echo 'a flash failed earlier ($MARK): the driver is not bound again' >&2; bound=0; fi
		trap 'rm -f $Q; [ \$bound = 1 ] && echo 1-005a > \$D/bind 2>/dev/null' EXIT
		trap 'exit 1' HUP INT TERM
		if [ \$bound = 1 ]; then echo 1-005a > \$D/unbind; fi
		$Q $CMD" && rc=0 || rc=$?
	;;
flash | recover)
	ARG=recover
	[ "$CMD" = flash ] && ARG=$4
	$SSH "$PANEL" "command -v setsid > /dev/null || { echo 'setsid missing on the panel' >&2; exit 3; }
		mkdir $R && date +%s > $R/started" || { echo "could not claim $R on the panel" >&2; exit 1; }
	if ! { put "$OUT" "$R/cstfw" && put "$HERE/cstfw-run.sh" "$R/cstfw-run.sh" && put "$3" "$R/image.bin"; }; then
		$SSH "$PANEL" "rm -rf $R" || true
		exit 1
	fi
	$SSH "$PANEL" "chmod +x $R/cstfw &&
		printf '%s\n' '$RUNNER_OWNER' > $LOCK/owner && date '+%Y-%m-%dT%H:%M:%S%z' > $LOCK/since || exit 1
		setsid sh $R/cstfw-run.sh '$ARG' < /dev/null > /dev/null 2>&1 &" && locked=0 || {
		echo "the runner was not started" >&2
		state || true
		exit 1
	}
	follow && rc=0 || rc=$?
	[ "$rc" = 0 ] || exit "$rc"
	;;
esac

if ! $SSH "$PANEL" "[ ! -e $MARK ]"; then
	state || true
	exit "${rc:-1}"
fi
sleep 3
$SSH "$PANEL" 'ls /sys/bus/i2c/drivers/Hynitron-TS/1-005a > /dev/null 2>&1 && echo "driver bound" || { echo "DRIVER NOT BOUND" >&2; exit 1; }
	pidof wallview-touch > /dev/null && echo "wallview-touch running" || echo "wallview-touch not running (resampling off?)"
	dmesg | tail -5'
exit "$rc"

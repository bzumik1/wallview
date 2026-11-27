#!/bin/sh
D=/sys/bus/i2c/drivers/Hynitron-TS
LOCK=/tmp/wallview-panel.lock
RUN=/tmp/cstfw.run
MARK=/data/wallview/cstfw-failed
RUNNER_OWNER="touch controller flash (cstfw-run)"
PIDS=/tmp/cstfw.agent-pids
ARG=$1

echo $$ > "$RUN/pid" || exit 1
trap '' HUP INT TERM PIPE

finish() {
	echo "$1" > "$RUN/rc.tmp" && mv "$RUN/rc.tmp" "$RUN/rc"
	exit 0
}

refuse() {
	echo "runner: $1; nothing touched" >> "$RUN/log"
	[ "$(cat "$LOCK/owner" 2>/dev/null)" = "$RUNNER_OWNER" ] && [ ! -e "$MARK" ] && rm -rf "$LOCK"
	finish 2
}

: > "$RUN/log" || exit 1
[ -n "$ARG" ] || refuse "no argument"
[ -x "$RUN/cstfw" ] && [ -s "$RUN/cstfw" ] || refuse "no cstfw binary"
[ -s "$RUN/image.bin" ] || refuse "no image"

bound=0
[ -e "$D/1-005a" ] && bound=1
had_mark=0
[ -e "$MARK" ] && had_mark=1
old_mark=$(cat "$MARK" 2>/dev/null)

line="running since $(date) ($ARG)"
mkdir -p "$(dirname "$MARK")" && printf '%s\n' "$line" > "$MARK" && sync && [ "$(cat "$MARK" 2>/dev/null)" = "$line" ] || {
	if [ "$had_mark" = 1 ]; then
		printf '%s\n' "$old_mark" > "$MARK" 2>/dev/null
		finish 2
	fi
	rm -f "$MARK"
	refuse "cannot write $MARK"
}
echo "runner: marker written, driver bound=$bound, earlier failure=$had_mark" >> "$RUN/log"

if [ ! -s "$PIDS" ]; then
	pidof wallview-agent > "$PIDS" 2>/dev/null
fi
for pid in $(cat "$PIDS" 2>/dev/null); do kill -STOP "$pid" 2>/dev/null; done
if [ "$bound" = 1 ]; then
	echo 1-005a > "$D/unbind" || echo "runner: unbind failed" >> "$RUN/log"
fi

if [ "$ARG" = recover ]; then
	"$RUN/cstfw" flash "$RUN/image.bin" --recover --erase-and-program >> "$RUN/log" 2>&1
else
	"$RUN/cstfw" flash "$RUN/image.bin" --current-checksum "$ARG" --erase-and-program >> "$RUN/log" 2>&1
fi
rc=$?
if [ "$rc" = 0 ] && ! grep -q '^cstfw: done and verified' "$RUN/log"; then
	echo "runner: cstfw returned 0 without its success line; treated as failed" >> "$RUN/log"
	rc=6
fi

ok=1
case "$rc" in
0)
	rm -f "$MARK"
	echo 1-005a > "$D/bind" || echo "runner: bind failed" >> "$RUN/log"
	;;
2 | 3 | 5)
	if [ "$had_mark" = 1 ]; then
		echo "rc $rc after an earlier failure ($old_mark), $(date)" > "$MARK"
		ok=0
	else
		rm -f "$MARK"
		if [ "$bound" = 1 ]; then
			echo 1-005a > "$D/bind" || echo "runner: bind failed" >> "$RUN/log"
		fi
	fi
	;;
*)
	echo "FAILED rc $rc $(date) ($ARG)" > "$MARK"
	ok=0
	;;
esac
sync
if [ "$ok" = 1 ]; then
	for pid in $(cat "$PIDS" 2>/dev/null); do kill -CONT "$pid" 2>/dev/null; done
	rm -f "$PIDS"
	[ "$(cat "$LOCK/owner" 2>/dev/null)" = "$RUNNER_OWNER" ] && rm -rf "$LOCK"
fi
rm -f "$RUN/image.bin"
finish "$rc"

#!/bin/sh
D=/sys/bus/i2c/drivers/Hynitron-TS
LOCK=/tmp/wallview-panel.lock
RUN=/tmp/cstfw.run
MARK=/data/wallview/cstfw-failed
RUNNER_OWNER="touch controller flash (cstfw-run)"
ARG=$1

echo $$ > "$RUN/pid"
trap '' HUP INT TERM PIPE

bound=0
[ -e "$D/1-005a" ] && bound=1
had_mark=0
[ -e "$MARK" ] && had_mark=1
if [ ! -s /tmp/cstfw.agent-pids ]; then
	pidof wallview-agent > /tmp/cstfw.agent-pids 2>/dev/null
fi
for pid in $(cat /tmp/cstfw.agent-pids 2>/dev/null); do kill -STOP "$pid" 2>/dev/null; done
[ "$bound" = 1 ] && echo 1-005a > "$D/unbind"
mkdir -p "$(dirname "$MARK")"
echo "running since $(date) ($ARG)" > "$MARK"
sync

if [ "$ARG" = recover ]; then
	/tmp/cstfw flash /tmp/cst-image.bin --recover --erase-and-program > "$RUN/log" 2>&1
else
	/tmp/cstfw flash /tmp/cst-image.bin --current-checksum "$ARG" --erase-and-program > "$RUN/log" 2>&1
fi
rc=$?

ok=1
case "$rc" in
0)
	rm -f "$MARK"
	echo 1-005a > "$D/bind"
	;;
2 | 3 | 5)
	if [ "$had_mark" = 1 ]; then
		echo "rc $rc after an earlier failure, $(date)" > "$MARK"
		ok=0
	else
		rm -f "$MARK"
		[ "$bound" = 1 ] && echo 1-005a > "$D/bind"
	fi
	;;
*)
	echo "FAILED rc $rc $(date) ($ARG)" > "$MARK"
	ok=0
	;;
esac
sync
rm -f /tmp/cst-image.bin
if [ "$ok" = 1 ]; then
	for pid in $(cat /tmp/cstfw.agent-pids 2>/dev/null); do kill -CONT "$pid" 2>/dev/null; done
	rm -f /tmp/cstfw.agent-pids
	[ "$(cat "$LOCK/owner" 2>/dev/null)" = "$RUNNER_OWNER" ] && rm -rf "$LOCK"
fi
echo "$rc" > "$RUN/rc.tmp" && mv "$RUN/rc.tmp" "$RUN/rc"

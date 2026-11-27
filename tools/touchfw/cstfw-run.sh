#!/bin/sh
D=/sys/bus/i2c/drivers/Hynitron-TS
LOCK=/tmp/wallview-panel.lock
RUN=/tmp/cstfw.run
MARK=/data/wallview/cstfw-failed
RUNNER_OWNER="touch controller flash (cstfw-run)"
PIDS=/tmp/cstfw.agent-pids
ARG=$1

trap '' HUP INT TERM PIPE

bail() {
	[ "$(cat "$LOCK/owner" 2>/dev/null)" = "$RUNNER_OWNER" ] && [ ! -e "$MARK" ] && rm -rf "$LOCK"
	exit 1
}

echo $$ > "$RUN/pid" || bail

finish() {
	echo "$1" > "$RUN/rc.tmp" && mv "$RUN/rc.tmp" "$RUN/rc"
	exit 0
}

refuse() {
	echo "runner: $1; nothing touched" >> "$RUN/log"
	[ "$(cat "$LOCK/owner" 2>/dev/null)" = "$RUNNER_OWNER" ] && [ ! -e "$MARK" ] && rm -rf "$LOCK"
	finish 2
}

: > "$RUN/log" || bail
[ -n "$ARG" ] || refuse "no argument"
[ -x "$RUN/cstfw" ] && [ -s "$RUN/cstfw" ] || refuse "no cstfw binary"
[ -s "$RUN/image.bin" ] || refuse "no image"
probe=$("$RUN/cstfw" 2>&1)
[ $? = 2 ] && case "$probe" in usage:*) true ;; *) false ;; esac || refuse "cstfw does not run here"
if [ "$ARG" != recover ] || [ ! -e "$MARK" ]; then
	awk '$2 == "/data" && $4 ~ /^rw/ { found = 1 } END { exit !found }' /proc/mounts || refuse "/data is not mounted read-write"
fi
[ -e "$MARK" ] && [ "$ARG" != recover ] && refuse "a flash failed earlier ($(cat "$MARK")): only recover"

bound=0
[ -e "$D/1-005a" ] && bound=1
had_mark=0
[ -e "$MARK" ] && had_mark=1
old_mark=$(cat "$MARK" 2>/dev/null)

line="running since $(date) ($ARG)"
mkdir -p "$(dirname "$MARK")" && printf '%s\n' "$line" > "$MARK" && sync && [ "$(cat "$MARK" 2>/dev/null)" = "$line" ] || {
	if [ "$had_mark" = 1 ] && [ -e "$MARK" ]; then
		echo "runner: marker not rewritten; the earlier one stands" >> "$RUN/log"
	else
		rm -f "$MARK"
		refuse "cannot write $MARK"
	fi
}
echo "runner: driver bound=$bound, earlier failure=$had_mark" >> "$RUN/log"

agent_pids=
for pid in $(cat "$PIDS" 2>/dev/null) $(pidof wallview-agent 2>/dev/null); do
	[ "$(cat "/proc/$pid/comm" 2>/dev/null)" = wallview-agent ] || continue
	case " $agent_pids " in *" $pid "*) ;; *) agent_pids="$agent_pids $pid" ;; esac
done
echo $agent_pids > "$PIDS"
for pid in $agent_pids; do kill -STOP "$pid" 2>/dev/null; done
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
rm -f "$RUN/image.bin"
if [ "$ok" = 1 ]; then
	for pid in $(cat "$PIDS" 2>/dev/null); do
		[ "$(cat "/proc/$pid/comm" 2>/dev/null)" = wallview-agent ] && kill -CONT "$pid" 2>/dev/null
	done
	rm -f "$PIDS"
fi
echo "$rc" > "$RUN/rc.tmp" && mv "$RUN/rc.tmp" "$RUN/rc"
if [ "$ok" = 1 ]; then
	[ "$(cat "$LOCK/owner" 2>/dev/null)" = "$RUNNER_OWNER" ] && rm -rf "$LOCK"
fi
exit 0

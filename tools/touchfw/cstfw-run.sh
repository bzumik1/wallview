#!/bin/sh
# Runs on the panel, detached with setsid, started by tools/touchfw/panel-cstfw.sh:
#   sh /tmp/cstfw-run.sh <current-checksum>|recover
# panel-cstfw.sh has created /tmp/cstfw.run (one run at a time), handed the panel
# lock to RUNNER_OWNER and copied /tmp/cstfw and /tmp/cst-image.bin.
# Results: /tmp/cstfw.run/{pid,log,rc}.
#
# Before the first attempt it writes the persistent marker /data/wallview/cstfw-failed
# (it survives a reboot). rc 0 removes it; rc 2/3/5 (nothing written) removes it
# only if it did not exist before. With rc 0/2/3/5 the driver is bound again (if it
# was bound and no failed flash is recorded), the agent resumes and the lock is
# released. Anything else leaves the marker, the driver unbound, the agent paused
# and the lock held by RUNNER_OWNER, so that only `panel-cstfw.sh ... recover` acts.
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
# Pause (not stop) the agent: a reboot from Home Assistant must not hit a running flash.
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

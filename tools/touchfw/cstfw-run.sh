#!/bin/sh
# Runs on the panel, detached with setsid, started by tools/touchfw/panel-cstfw.sh:
#   sh /tmp/cstfw-run.sh <current-checksum>|recover '<panel lock owner>'
# panel-cstfw.sh has created /tmp/cstfw.run (one run at a time) and copied
# /tmp/cstfw and /tmp/cst-image.bin. Results: /tmp/cstfw.run/{pid,log,rc}.
#
# rc 0 (done) and 2/3 (refused, nothing written): the driver is bound again if it
# was bound and no earlier flash failed, the agent continues, the lock is released.
# Any other rc: /tmp/cstfw.failed is created, the driver stays unbound, the agent
# stays paused (it could reboot the panel) and the lock stays held. Recover with
# `panel-cstfw.sh <panel> recover <vendor image> --erase-and-program`.
D=/sys/bus/i2c/drivers/Hynitron-TS
LOCK=/tmp/wallview-panel.lock
RUN=/tmp/cstfw.run
ARG=$1
OWNER=$2

echo $$ > "$RUN/pid"
trap '' HUP INT TERM PIPE

bound=0
[ -e "$D/1-005a" ] && bound=1
# Pause (not stop) the agent: a reboot from Home Assistant must not hit a running flash.
if [ ! -s /tmp/cstfw.agent-pids ]; then
	pidof wallview-agent > /tmp/cstfw.agent-pids 2>/dev/null
fi
for pid in $(cat /tmp/cstfw.agent-pids 2>/dev/null); do kill -STOP "$pid" 2>/dev/null; done
[ "$bound" = 1 ] && echo 1-005a > "$D/unbind"

if [ "$ARG" = recover ]; then
	/tmp/cstfw flash /tmp/cst-image.bin --recover --erase-and-program > "$RUN/log" 2>&1
else
	/tmp/cstfw flash /tmp/cst-image.bin --current-checksum "$ARG" --erase-and-program > "$RUN/log" 2>&1
fi
rc=$?

ok=1
case "$rc" in
0)
	rm -f /tmp/cstfw.failed
	echo 1-005a > "$D/bind"
	;;
2 | 3)
	if [ ! -e /tmp/cstfw.failed ] && [ "$bound" = 1 ]; then
		echo 1-005a > "$D/bind"
	fi
	[ -e /tmp/cstfw.failed ] && ok=0
	;;
*)
	echo "rc $rc $(date)" > /tmp/cstfw.failed
	ok=0
	;;
esac
rm -f /tmp/cst-image.bin
if [ "$ok" = 1 ]; then
	for pid in $(cat /tmp/cstfw.agent-pids 2>/dev/null); do kill -CONT "$pid" 2>/dev/null; done
	rm -f /tmp/cstfw.agent-pids
	[ "$(cat "$LOCK/owner" 2>/dev/null)" = "$OWNER" ] && rm -rf "$LOCK"
fi
echo "$rc" > "$RUN/rc.tmp" && mv "$RUN/rc.tmp" "$RUN/rc"

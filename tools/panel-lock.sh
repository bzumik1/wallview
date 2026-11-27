#!/bin/sh
# Panel lock shared by all agents (CLAUDE.md "The panel").
#   tools/panel-lock.sh root@PANEL acquire "<agent> · <task>"   exit 75 if held
#   tools/panel-lock.sh root@PANEL release "<same owner>"        only the holder releases
#   tools/panel-lock.sh root@PANEL status
# mkdir is atomic, so two agents cannot both get it; /tmp is cleared on reboot.
PANEL=$1
ACTION=$2
LOCK=/tmp/wallview-panel.lock
[ -n "$PANEL" ] && [ -n "$ACTION" ] || { echo "usage: $0 root@PANEL acquire <owner>|release <owner>|status" >&2; exit 2; }
case "$ACTION" in
acquire)
	[ -n "$3" ] || { echo "acquire needs an owner, e.g. \"claude · touch firmware\"" >&2; exit 2; }
	OWNER=$(printf '%s' "$3" | tr -d "'")
	ssh "$PANEL" "if mkdir $LOCK 2>/dev/null; then echo '$OWNER' > $LOCK/owner; date '+%Y-%m-%dT%H:%M:%S%z' > $LOCK/since; echo 'panel lock acquired'; else echo \"panel lock held by: \$(cat $LOCK/owner 2>/dev/null) since \$(cat $LOCK/since 2>/dev/null)\" >&2; exit 75; fi"
	;;
release)
	[ -n "$3" ] || { echo "release needs the owner given to acquire" >&2; exit 2; }
	OWNER=$(printf '%s' "$3" | tr -d "'")
	ssh "$PANEL" "if [ \"\$(cat $LOCK/owner 2>/dev/null)\" = '$OWNER' ]; then rm -rf $LOCK && echo 'panel lock released'; else echo \"not released, held by: \$(cat $LOCK/owner 2>/dev/null)\" >&2; exit 75; fi"
	;;
status)
	ssh "$PANEL" "if [ -d $LOCK ]; then echo \"held by: \$(cat $LOCK/owner 2>/dev/null) since \$(cat $LOCK/since 2>/dev/null)\"; else echo free; fi"
	;;
*)
	echo "unknown action $ACTION" >&2
	exit 2
	;;
esac

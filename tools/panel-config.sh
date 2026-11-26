#!/bin/bash
set -euo pipefail

usage() {
	echo "usage: panel-config.sh <panel-ip> show" >&2
	echo "       panel-config.sh <panel-ip> url <dashboard-url>" >&2
	echo "       panel-config.sh <panel-ip> language <locale>" >&2
	echo "       panel-config.sh <panel-ip> cog-args <arguments>" >&2
	echo "       panel-config.sh <panel-ip> mqtt <host> <port> <username> <password-file>" >&2
	exit 2
}

PANEL=${1:-}
ACTION=${2:-}
[ -n "$PANEL" ] && [ -n "$ACTION" ] || usage
SSH_OPTIONS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10)

panel() {
	ssh "${SSH_OPTIONS[@]}" "root@$PANEL" "$@"
}

write_setting() {
	panel "mkdir -p /data/wallview && cat > /data/wallview/$1"
}

case "$ACTION" in
show)
	panel 'for file in url language cog-args; do printf "%-9s %s\n" "$file" "$(cat /data/wallview/$file 2>/dev/null)"; done; printf "%-9s %s\n" mqtt "$(sed -n "s/^host=//p" /data/wallview/mqtt.conf 2>/dev/null)"'
	;;
url | language | cog-args)
	[ $# -ge 3 ] || usage
	printf '%s\n' "$3" | write_setting "$ACTION"
	panel 'killall cog 2>/dev/null; true'
	;;
mqtt)
	[ $# -ge 6 ] || usage
	{
		echo "host=$3"
		echo "port=$4"
		echo "username=$5"
		echo "password=$(tr -d '\r\n' < "$6")"
	} | write_setting mqtt.conf
	panel 'chmod 600 /data/wallview/mqtt.conf; killall wallview-agent 2>/dev/null; [ -x /etc/init.d/S60wallview-agent ] && (pidof wallview-agent >/dev/null || /etc/init.d/S60wallview-agent start); true'
	;;
*)
	usage
	;;
esac

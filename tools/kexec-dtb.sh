#!/bin/bash
set -euo pipefail

RUNNING=${1:?usage: kexec-dtb.sh <running.dtb> <new.dtb> <output.dtb>}
NEW=${2:?usage: kexec-dtb.sh <running.dtb> <new.dtb> <output.dtb>}
OUTPUT=${3:?usage: kexec-dtb.sh <running.dtb> <new.dtb> <output.dtb>}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cp "$NEW" "$work/merged.dtb"
memory_nodes=$(fdtget -l "$RUNNING" / | grep -E '^memory(@|$)' || true)
[ -n "$memory_nodes" ] || { echo "no memory node in $RUNNING" >&2; exit 1; }

for node in $memory_nodes; do
	fdtget -l "$work/merged.dtb" / | grep -qx "$node" || fdtput -c "$work/merged.dtb" "/$node"
	fdtput -t s "$work/merged.dtb" "/$node" device_type memory
	fdtput -t x "$work/merged.dtb" "/$node" reg $(fdtget -t x "$RUNNING" "/$node" reg)
done

dtc -q -I dtb -O dts "$RUNNING" | grep '^/memreserve/' > "$work/memreserve" || true
dtc -q -I dtb -O dts "$work/merged.dtb" > "$work/merged.dts"
{
	head -n 1 "$work/merged.dts"
	cat "$work/memreserve"
	tail -n +2 "$work/merged.dts"
} > "$work/final.dts"
dtc -q -I dts -O dtb -o "$OUTPUT" "$work/final.dts"

echo "memory: $memory_nodes, memreserve entries: $(wc -l < "$work/memreserve")"

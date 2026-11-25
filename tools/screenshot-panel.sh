#!/bin/bash
set -euo pipefail

PANEL=${1:?usage: screenshot-panel.sh <panel-ip> <output.png>}
OUTPUT=${2:?usage: screenshot-panel.sh <panel-ip> <output.png>}
SSH_OPTIONS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10)
RAW=$(mktemp)
trap 'rm -f "$RAW"' EXIT

ssh "${SSH_OPTIONS[@]}" "root@$PANEL" 'cat /dev/fb0' > "$RAW"
cp "$RAW" "$(dirname "$OUTPUT")/.fb0.raw"
limactl shell wallview -- python3 -c '
import sys
from PIL import Image
raw, output = sys.argv[1], sys.argv[2]
data = open(raw, "rb").read()[:480 * 480 * 4]
Image.frombytes("RGB", (480, 480), data, "raw", "BGRX").save(output)
' "$(cd "$(dirname "$OUTPUT")" && pwd)/.fb0.raw" "$(cd "$(dirname "$OUTPUT")" && pwd)/$(basename "$OUTPUT")"
rm -f "$(dirname "$OUTPUT")/.fb0.raw"

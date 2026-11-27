# Touch controller captures

Raw CST226SE reads recorded with `tools/perf/panel-touchprobe.sh` (driver unbound, vendor firmware `0x99630e41`).

Line format, as written by `tools/perf/touchprobe.c`:

- `F <ns>`: INT falling edge
- `R <ns>`: INT rising edge
- `D <read start ns> <read end ns> <points> <status> <x> <y> <check>`: one read of `0xD000`; `points` is byte 5 & 0x7f, `status` is (byte 0 & 0x0f) >> 1 (3 = pressed), `check` is byte 6 (171 = `0xAB`)
- `E <ns>`: read error

`touch-raw-2026-11-27.txt` (120 s): resting finger 0–18 s, slow circles 19–46 s, about 25 flicks with lifts 47–68 s, about 20 taps 72–83 s, 7 long presses 84–97 s, then no touch.

`touch-raw-2026-11-27-partial.txt` (120 s): 22 s of circling, 13 flicks, then no touch.

`C <ns> <count> <ok>` lines (touchprobe `counter<ms>`): the controller frame counter at `0xD040`; `ok` 1 when its check value matches; `CE <ns>` a failed counter read.

`touch-counter-idle-2026-11-27.txt` (30 s, no touch): 83 frames/s, no INT.
`touch-counter-finger-2026-11-27.txt` (45 s): resting finger about 38 frames/s (INT about 38/s), slow circles 48–53 frames/s (INT 42–44/s).
`touch-counter-poll12-2026-11-28.txt` (30 s, no touch, `0xD000` read and acknowledged every 12 ms): 83.5 frames/s.
`touch-counter-idle-2026-11-28.txt` (30 s, no touch, counter only, control): 83.5 frames/s.

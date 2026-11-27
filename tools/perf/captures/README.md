# Touch controller captures

Raw CST226SE reads recorded with `tools/perf/panel-touchprobe.sh` (driver unbound, vendor firmware `0x99630e41`).

Line format, as written by `tools/perf/touchprobe.c`:

- `F <ns>`: INT falling edge
- `R <ns>`: INT rising edge
- `D <read start ns> <read end ns> <points> <status> <x> <y> <check>`: one read of `0xD000`; `points` is byte 5 & 0x7f, `status` is (byte 0 & 0x0f) >> 1 (3 = pressed), `check` is byte 6 (171 = `0xAB`)
- `E <ns>`: read error

`touch-raw-2026-11-27.txt` (120 s): resting finger 0–18 s, slow circles 19–46 s, about 25 flicks with lifts 47–68 s, about 20 taps 72–83 s, 7 long presses 84–97 s, then no touch.

`touch-raw-2026-11-27-partial.txt` (120 s): 22 s of circling, 13 flicks, then no touch.

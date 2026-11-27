# Touch controller (Hynitron CST226SE): firmware, update protocol, report rate

Investigation of 2026-11-27. Everything here comes from static analysis of the vendor Android kernel and of the firmware image embedded in it. Nothing was written to the controller, and the controller was not read either (see "Live chip read"). Flashing plans at the end are **not executed**.

## Summary

- The vendor kernel contains the complete CST226SE firmware (7680 bytes, version `0x03000001`, checksum `0x99630e41`) and the code that flashes it. Image, checksum algorithm and update protocol are fully reconstructed.
- The chip is a Cortex-M0 with 7.5 KB of rewritable flash (`0x0000–0x1DFF`) and a separate, non-updatable code memory at about `0x8000–0xE000` that holds the scan engine, the I²C command dispatcher and, by all evidence, the `0xA0xx` bootloader. A bad or interrupted flash should therefore be recoverable by flashing again. This is strong static evidence, not yet a proof on hardware.
- There is no register or command for the report rate in the flash part of the firmware, in the vendor driver or in the mainline driver. The main loop is free-running: one frame = one scan in ROM + processing. The rate is set by scan duration, i.e. by analog front-end parameters in the configuration table at flash `0x1604`, whose meaning is not documented. Reaching 120 Hz means guessing which bytes shorten the scan and testing each guess with a re-flash.
- Recommendation: do not start with flashing. Resampling in `wallview-touch` costs nothing and has no risk; flashing is an experiment with unknown outcome even when recovery works.

## Status and how to resume

Parked on 2026-11-27 by the owner's decision: touch smoothness is handled by resampling (`buildroot/board/wallview/touch/wallview-touch.c`), the firmware route stays open for later. Measured controller rate and the measurement method are in [performance.md](performance.md#touch-controller-report-rate-2026-11-27) (`tools/perf/touchprobe.c`, `tools/perf/touchprobe.py`).

To resume a higher report rate, in this order, each step only with the owner's explicit approval:

1. Live chip read ("Live chip read" below) with the driver unbound and nothing else holding the touchscreen (stop `wallview-touch` first, it exits when the device disappears): confirm version `0x03000001` and checksum `0x99630e41`.
2. Plan (a) step 1: bootloader entry dry run (reset, `A0 01 AA`, `0xA003 == 0x55`, reset), no erase.
3. Plan (a): no-op re-flash of the original image from `local/firmware/hynitron/cst226se-vendor-0x03000001.bin`, twice.
4. Plan (b): one configuration byte per image, checksum recomputed, measured with `touchprobe` (new position interval, INT period) and checked for noise and ghost touches.

The vendor firmware is not distributed: it stays in the git-ignored `local/firmware/` and `extract/`.

## Tools (`tools/touchfw/`)

Written 2026-11-27 for the 120 Hz goal and reviewed in rounds by an independent senior embedded reviewer. Run on the development panel on 2026-11-27: info, boot-check, boot-checksum and two no-op re-flashes of the vendor image, all rc 0.

- `cstfw.c` (runs on the panel, refuses while the kernel driver is bound):
  - `info`: step 1, trailer registers in info mode.
  - `boot-check`: step 2, reset, `A0 01 AA`, `0xA003 == 0x55`, reset. Sends no erase, page or data command.
  - `boot-checksum`: as `boot-check` plus `0xA000`/`0xA008`; expect "no checksum available" (the vendor reads them only after programming).
  - `flash <image> --current-checksum <0x...> --erase-and-program`: plan (a)/(b), the vendor sequence from the table below, 5 attempts from page 0. Reads the chip first and refuses unless its identification (`0xD1F4–0xD207`) equals the image's and `0xD20C` equals `--current-checksum`. After programming it checks that the new application answers with the image's checksum.
  - `flash <vendor image> --recover --erase-and-program`: the same without reading the chip first, for a chip whose application no longer answers; only the vendor image is accepted.
  - From the first reset on, flash ignores HUP, INT, TERM, QUIT and PIPE. Reads are write+STOP+read like the vendor driver. The reset line is used only if `/dev/gpiochip0` is `gpio0` and line 12 is free.
  - Exit codes: 0 done (prints `cstfw: done and verified`); 2 refused before touching the chip; 3 refused after reading it (nothing written); 5 boot mode never reached (nothing written); 1 programming failed (flash may be partly written); 4 programmed but the new application does not answer as expected (checked three times over about 1 s).
- `panel-cstfw.sh root@<panel> info|boot-check|boot-checksum|flash <image> <checksum> --erase-and-program|recover <vendor image> --erase-and-program|follow|state` (Mac):
  - builds `cstfw` in the Lima VM into a fresh temporary file under `out/cstfw/` and takes the panel lock (`tools/panel-lock.sh`) before it touches anything on the panel. Every upload is checked with sha256 on both sides.
  - info/boot-check/boot-checksum unbind the driver only if bound and bind it again (remote trap, also on a dropped connection) only if it was bound and no flash has failed.
  - flash/recover claim `/tmp/cstfw.run/` under the lock, upload `cstfw`, the runner and the image into it, then hand the panel lock to the runner (owner `touch controller flash (cstfw-run)`) in the same command that starts it detached (`setsid`). From then on the Mac side cannot release it; losing SSH or Ctrl-C cannot stop or unlock the flash; `follow` attaches again, `state` prints marker, lock, driver, paused agent and run.
  - A run directory is stale (its log is kept as `/data/wallview/cstfw-last.log`, then it is removed under the lock) when it has a result, or its runner pid is dead, or it has no pid and was claimed more than 120 s of uptime ago (or has no claim time).
  - While the failed-flash marker exists, `flash`, `boot-check` and `boot-checksum` refuse (wrapper and runner); only `recover` may touch the chip. `recover` accepts only the vendor image (sha256 checked on the Mac).
  - The lock owner includes the wrapper's pid and start time, so two wrappers on one Mac never share it. During a takeover the lock owner stays the runner's; the taking-over `recover` holds the symlink `takeover` inside the lock (created atomically, with an uptime stamp refreshed after each upload). A takeover older than 300 s with no live run is replaced by the next `recover`, so a lost connection cannot strand the lock. For a recover with an existing marker the runner skips the `/data` checks (the old marker stands). The runner writes its result before it releases the lock.
  - Before it changes anything the runner checks that `cstfw` runs (no arguments: rc 2 and `usage:`), that `/data` is mounted read-write, and writes the persistent marker `/data/wallview/cstfw-failed` and reads it back; if any of this fails it stops with rc 2 and releases the lock. Then it pauses the agent, only pids whose `comm` is `wallview-agent` (SIGSTOP, so a reboot from Home Assistant cannot hit the flash; the hardware watchdog is fed by the BusyBox daemon, not the agent) and unbinds the driver.
  - Success needs rc 0 and the line `cstfw: done and verified` in the log; rc 0 without it counts as failed (rc 6). On success, or on 2/3/5 without an earlier failure, it removes the marker, binds the driver (if it was bound), resumes the agent and releases the lock. Otherwise the marker stays, the driver unbound, the agent paused and the lock held by the runner; only `recover` acts then, taking over a runner-held lock whenever no run is live (marker or not).
  - After a reboot the lock and the pauses are gone and the kernel binds the driver again, but the marker stays: `recover` works the same through the normal lock and unbinds the driver itself. Do not use touch in between.
- `cstimg.py show|set`: shows the suspected front-end fields of an image (with sha256 and whether it is the vendor image), or writes a new file (never the input or an existing file) with changed bytes of the main configuration table `0x1604–0x16cb` and a recomputed checksum; version unchanged.

Way back by hand, if the tools themselves get stuck (on the panel):
1. `cat /data/wallview/cstfw-failed /tmp/cstfw.run/log`; check that the runner is gone (`kill -0 $(cat /tmp/cstfw.run/pid)` fails) and no `cstfw` runs (`pidof cstfw`). A `cstfw` still running after 5 minutes is hung (five attempts on a stuck bus take about 3 minutes): `kill -9 $(pidof cstfw)`.
2. If the marker exists, or anything was written: leave the agent paused, the driver unbound and the lock as they are, make sure the marker exists (`echo manual > /data/wallview/cstfw-failed`), and run `recover` from the Mac (it always needs the Mac, the vendor image and the Lima VM). `recover` takes over a runner-held lock itself, also one with a `takeover` link older than 5 minutes; do not remove that link by hand while a `recover` may run.
3. Only if nothing was written (rc 2, 3 or 5, or no rc at all, and no marker): `rm -rf /tmp/cstfw.run`, `kill -CONT $(cat /tmp/cstfw.agent-pids)`, `rm -f /tmp/cstfw.agent-pids`, `echo 1-005a > /sys/bus/i2c/drivers/Hynitron-TS/bind`, and remove the lock only if its owner is `touch controller flash (cstfw-run)`.

What must be true before `flash` is ever run (the revert guarantee):

1. `boot-check` passed on this panel with the same `cstfw` build, and `info` after it shows the seven expected rows.
2. The vendor image exists in two places and its sha256 (`0496946b…bbf1`) is checked right before flashing.
3. `0xD20C` is served from the flash trailer, not computed, so it does not prove the content. Only a boot-loader checksum (`boot-checksum`, if it gives one) or the vendor kernel's behaviour supports it. Done 2026-11-27 on the development panel: in boot mode, before any programming, `0xA000 = 01` and `0xA008` = 0x99630e41, the vendor image's checksum, so the boot loader reports a checksum of the flash content without being programmed first.
4. The page-select encoding (`A0 14 <lo> <hi>`) and the data registers `0xA018 + 4k` (up to `0xA214`) have been checked against the vendor disassembly by a second reader. Done 2026-11-27: an independent reading of the vendor `Image` (routine at file offset 0x6342f0) matches the table and `cstfw.c` byte for byte, and the image embedded in that kernel has the vendor image's sha256. The vendor I²C helpers retry every transaction up to 5 times; `cstfw` does the same during programming. The vendor ignores the results of `A0 06 00` and `A0 03 00`; `cstfw` logs them and continues, the `0xA000`/`0xA008` check decides. Worst case on a stuck bus is now about 3 minutes. What `0xA000`/`0xA008` return before programming (`boot-checksum`) cannot be derived from the vendor code.
5. The vendor Android `boot.img` has been booted once on the development panel (plan (a) step 6 fallback). Not done: booting Android restores the stock `recovery`, and coming back needs Loader mode over USB, which Jakub keeps as the last resort (2026-11-27). The way back is therefore, in order: `cstfw` retries and `recover`; `wallview-boot android` and a reboot, so the vendor driver re-flashes at probe; USB Loader mode to return to WallView; a replacement controller.
6. Stable power, nothing else using the panel. The first flash is the unmodified vendor image. Jakub accepts that recovery is inferred until that first re-flash has succeeded. Done 2026-11-27: two no-op re-flashes of the vendor image on the development panel, both rc 0 on the first attempt, boot-loader checksum 0x99630e41 and the same seven info rows after each.

Typical session, each step with the owner's approval:

```
tools/touchfw/panel-cstfw.sh root@PANEL info
tools/touchfw/panel-cstfw.sh root@PANEL boot-check
tools/touchfw/panel-cstfw.sh root@PANEL boot-checksum
sha256sum local/firmware/hynitron/cst226se-vendor-0x03000001.bin
tools/touchfw/panel-cstfw.sh root@PANEL flash local/firmware/hynitron/cst226se-vendor-0x03000001.bin 0x99630e41 --erase-and-program   # twice
tools/touchfw/cstimg.py set local/firmware/hynitron/cst226se-vendor-0x03000001.bin local/firmware/hynitron/try-166a-10.bin 0x166a=10
tools/touchfw/panel-cstfw.sh root@PANEL flash local/firmware/hynitron/try-166a-10.bin 0x99630e41 --erase-and-program
# measure with tools/perf/touchprobe; back to the vendor image with --current-checksum of the try image
# after a failed flash (driver unbound, lock held by the failed run):
tools/touchfw/panel-cstfw.sh root@PANEL recover local/firmware/hynitron/cst226se-vendor-0x03000001.bin --erase-and-program
```

## Where the firmware is

The vendor kernel (`extract/boot.img` → uncompressed arm64 `Image`, Linux 4.4.138) has `CONFIG_KALLSYMS_ALL`, so symbols are recoverable:

```
python3 -m venv venv && venv/bin/pip install capstone git+https://github.com/marin-m/vmlinux-to-elf
venv/bin/kallsyms-finder Image > kallsyms.txt        # base 0xffffff8008080000
grep -E "hyn|cst[23]xx|fwbin" kallsyms.txt
```

Two Hynitron drivers are built in, each with its own static `fwbin` array:

| Driver (`i2c_device_id`) | DT compatible, address | `fwbin` symbol | Size | Content |
|---|---|---|---|---|
| `cst2xx` | `HYN,CST226`, 0x1a | `0xffffff800926c358` | 6168 | other chip (not Cortex-M code), updated by `cst2xx_boot_update_fw` (`0xD110` protocol) |
| `cst226se` | `HYN,CST226SE`, 0x5a | `0xffffff800926dde0` | 7680 | **our chip**, Cortex-M0 image, updated inline in `cst2xx_check_code` at `0xffffff80086b42f0` |

File offset in the `Image` = address − `0xffffff8008080000`, so the CST226SE image is `dd if=Image bs=1 skip=$((0x11EDDE0)) count=7680`.

Extracted copies (git-ignored, never commit):

- `local/firmware/hynitron/cst226se-vendor-0x03000001.bin`, sha256 `0496946b4abe9cf9ad1760011570614d2612c88614cd72fdc75b92b5ac4cbbf1`
- `local/firmware/hynitron/cst226-addr1a-vendor.bin` (the other chip, for reference only)

No Hynitron firmware file was looked for in `system.img`/`vendor.img`/`oem.img`; the driver uses only the static array.

## Image layout

| Offset | Content |
|---|---|
| `0x0000` | Cortex-M0 vector table: SP `0x2000036c`, reset `0x0435`, HardFault `0x0429`, IRQ0–4 at `0xf9`, `0xfb`, `0x11b`, `0x137`, `0x14b` |
| `0x0054–0x1603` | application code (Thumb) |
| `0x1604–0x196b` | configuration: main table at `0x1604`, tables at `0x16cc` (register pointers), `0x1804`, `0x1824`, `0x1836`, `0x1848`, `0x1924`, scatter-load table at `0x1950` |
| `0x196c–0x1dd3` | zero |
| `0x1dd4` | `5b 5a 35 35 a5 a5 ca ca 88 77 66 55 44 33 22 11` (markers) |
| `0x1de4` | `0d 00 08 00`: 13 TX, 8 RX (register `0xD1F4`) |
| `0x1de8` | `e0 01 e0 01`: 480 × 480 (`0xD1F8`) |
| `0x1dec` | `dc 5f ca ca`: check code `0xcaca5fdc` (`0xD1FC`) |
| `0x1df0` | `02 03 00 00` (`0xD200`) |
| `0x1df4` | `01 4b a8 00`: project `0x4b01`, chip type `0x00a8` (`0xD204`) |
| `0x1df8` | version `0x03000001`, little endian (`0xD208`) |
| `0x1dfc` | checksum `0x99630e41`, little endian (`0xD20C`) |

The firmware serves info-mode reads of `0xD1F0–0xD213` straight from flash `0x1DE0 + (reg − 0xD1F0)` and `0xD220–0xD240` from `0x1DC0 + (reg − 0xD220)` (handler at `0x0d58`), which is why the trailer and the registers match.

Checksum = `0x55` + sum of the 1919 little-endian 32-bit words at `0x0000–0x1DFB`, modulo 2³²:

```python
import struct
d = open("cst226se-vendor-0x03000001.bin", "rb").read()
s = (0x55 + sum(struct.unpack("<1919I", d[:0x1dfc]))) & 0xffffffff
assert s == struct.unpack("<I", d[0x1dfc:])[0] == 0x99630e41
```

A modified image only needs this word recomputed. The same value is what the bootloader returns at `0xA008` after programming.

## Vendor update protocol (from `cst2xx_check_code`)

All registers are 16-bit, high byte first. Reads are a 2-byte write followed by a separate read.

Decision (normal mode, after writing `D1 06` and 10 ms):

1. read `0xD049` (2 bytes): customer id, only logged
2. read `0xD20C` (4): chip checksum. Equal to image `[0x1DFC]` → done, no update
3. read `0xD208` (4): chip version. Greater than image `[0x1DF8]` → done ("bin_version is lower")
4. otherwise update

Update (up to 5 attempts, IRQ disabled, ESD work stopped):

| Step | Bus traffic | Timing |
|---|---|---|
| reset | reset low 10 ms, high | wait 10 ms (`msleep`, so 10–20 ms) |
| enter boot mode | write `A0 01 AA` | wait 6 ms |
| check | read `0xA003` (1) → must be `0x55` | on failure repeat write+check up to 30 times without another reset |
| clear update key | write `A0 06 00` | |
| per 512-byte page, 15 pages, offsets `0x0000…0x1C00` | write `A0 14 00 <offset high byte>` | |
| | 128 writes of `A0xx d0 d1 d2 d3`, register `0xA018 + 4·k`, k = 0…127 | |
| | write `A0 04 EE` (erase + program page) | wait 600 ms |
| | read `0xA005` (1) → `0x55` | up to 10 tries, 100 ms apart |
| finish | write `A0 01 00`, write `A0 03 00` | wait 10 ms |
| checksum | read `0xA000` (1) → must be `0x01`, else "No checksum" | up to 10 tries, then 4 ms |
| | read `0xA008` (4, little endian) → must equal image `[0x1DFC]` | |
| exit | write `A0 06 EE` | wait 100 ms |
| reset | reset low 10 ms, high | wait 30 ms |

The whole 7680 bytes are written, trailer included; nothing in the flash is preserved, so the image carries no per-unit data. Total time is about 15 × 0.75 s ≈ 12 s. Differences from the CST3xx path in mainline `hynitron_cstxxx.c`: boot check is `0xA003 == 0x55` instead of `0xA002 == 0xAC` (this is why `docs/test-log.md` saw `0xa002` read 0), pages are 512 bytes sent as 4-byte register writes instead of 1 KB blocks.

Other commands the vendor driver sends: `D1 09` (normal mode, before each ESD check), read `0xD040` (6 bytes: 32-bit frame counter big endian + 16-bit sum + `0xA5`; the ESD check resets the chip when it stops counting), `D1 05` on suspend (deep sleep), reset + `D1 06` on resume.

## Is the bootloader always reachable?

Evidence that it lives outside the rewritable flash:

1. The image covers the whole flash from the vector table at `0x0000` to `0x1DFF`, and the updater rewrites all 15 pages starting with page 0. No page is kept, so the code that receives the pages cannot be in them.
2. The application calls code that is not in the image: direct calls to `0x8690`, `0x88ce`, `0x8a28`, `0x8f4a`, `0x95a4`, `0x98ac`, `0x9ad0`, `0x9cb4`, `0xd516` and 22 entries of a jump table at `0xDF20–0xDFE8`. A separate code memory of at least 24 KB exists and is never updated.
3. The application's I²C handler (`0x0d58`) decodes only `0xD000–0xD050`, `0xD100–0xD120`, `0xD1F0–0xD213`, `0xD220–0xD240`. Nothing in the flash handles `0xA0xx`.
4. Boot mode is entered by reset + `A0 01 AA` only, before the application runs; the application takes no part. The vendor driver retries the complete procedure from page 0 after a failed boot entry, page write or acknowledge, `A0 01 00` or `A0 06 EE` (not after a checksum failure, where it gives up), which only makes sense if entry does not depend on valid flash content.
5. The checksum at `0xA008` is computed by the bootloader over the flash, not supplied by the host.

Not known: how long after reset the boot code listens for `A0 01 AA` (the vendor driver first tries 10–20 ms after releasing reset and keeps trying for about 200 ms; mainline CST3xx uses 7–11 ms), whether the boot code refuses to start an application with a bad checksum, and whether the "ROM" is mask ROM or protected flash. None of these change the recovery argument, but until a re-flash has been done once on this hardware, recovery is inferred, not demonstrated.

## Firmware structure and I²C map (application mode)

- `0x01ce` init: stores the configuration pointer `0x1604` at `0x20000ae8`, stores five callback pointers for the ROM, calls ROM init functions.
- `0x02d8` main loop, one iteration per frame: ROM scan and processing (`0xdfb8` … `0xdf88`), noise statistics over the 13 × 8 node array (`0x025c`, `0x04f2`, `0x068a`), report (`0x0568`), then `0x0470` waits (`wfi`) for the tick flag at `0x20000aa0`.
- Interrupts: IRQ1 `0x00fa` is a watchdog-style timer at `0x40000200` (counts ticks, stops feeding after 24 ticks without a frame; ROM call `0xdfd8(60)` sets its period at init), IRQ2 `0x011a` probably wake-up (sets the tick flag), IRQ3 `0x0137` → ROM scan interrupt, IRQ4 `0x014b`/`0x0d58` I²C slave.
- The INT pin (GPIO bit `cfg[0x26]` = 2 of port `0x40001000`) goes low in `0x0568` when a report is ready and high again in the next iteration after the scan (`0x0368`). INT-low time therefore is roughly one scan, which matches the measured 14 ms low out of an 18.5 ms period.

| Register | Access | Meaning |
|---|---|---|
| `0xD000–0xD050` | read/write | report buffer in RAM (`0x20000010`); `0xD000…` touch data, `0xD006 = 0xAB` marker, host writes `0xD000 = 0xAB` to acknowledge |
| `0xD040` | read | frame counter + checksum (ESD check) |
| `0xD100–0xD120` | write | commands, dispatched in ROM (`0x8a28`), list unknown. Known from drivers: `01` info mode, `05` deep sleep, `06` (vendor: after reset), `09` normal mode. `0xD10E` additionally forces a watchdog reset (visible in flash at `0x0de2`) |
| `0xD1F0–0xD213`, `0xD220–0xD240` | read (info mode) | flash trailer, see layout table |

## Report rate

Measured (`docs/performance.md`): new coordinates every 13–20 ms with gaps to 45–55 ms, not on a fixed grid.

What the firmware shows:

- No timer in the flash code sets a frame period. The loop waits for a tick only at the end of a frame, and intervals are not multiples of anything, so the loop is free-running and the period is scan time plus processing. The noise logic in `0x068a` (a ROM delay call with argument 5000 when a noise condition is met) was first suspected for the irregular long gaps; the detailed reading in "Noise logic and the cost of a touched frame" below shows it costs too little and too rarely, and that `0x20000008` is a statistic, not a threshold.
- The scan itself is ROM code driven by the configuration table. The flash code copies these fields into the analog front-end registers at `0x40001080` (function `0x0180`):

| Config offset (file offset) | Value | Goes to |
|---|---|---|
| `0x64` (`0x1668`) | `0x06` | `0x40001088` low byte |
| `0x66` (`0x166a`) | `0x14` (20) | `0x40001088` high byte |
| `0x65` (`0x1669`) | `0x04` | `0x4000108a` low byte |
| `0x9c` (`0x16a0`) | `0x00` | `0x4000108a` high byte; when non-zero `0x40001082` = `0x122` instead of `0x102` |
| `0xb4` (`0x16b8`) | `0x0153` (339) | `0x40001086` |
| `0x67`, `0x68` (`0x166b`, `0x166c`) | `0x0a`, `0x0c` | `0x4000106e` |

These are the prime suspects for drive pulse count, integration time and clock divider per TX line (13 lines; 14 ms per scan is about 1.1 ms per line). Which is which is **not known**; there is no public register description for this front end, and the sister-chip documents read on 2026-11-27 (see "Findings of 2026-11-27") do not describe the front end either. The rest of the table is consumed by the ROM.

- No runtime switch: the vendor driver, mainline and the flash code contain no "report rate", "game mode" or "high rate" command. If one exists it is one of the undocumented `0xD100–0xD120` commands inside the ROM.

What 120 Hz would take and cost: the frame has to drop from about 14–18 ms to 8.3 ms, so the scan must be roughly halved. In any mutual-capacitance front end that means fewer drive pulses or shorter integration per line, and signal-to-noise falls roughly with the square root of the integration time (halving it costs about 3 dB), on a panel whose firmware already spends effort on noise handling. Expected side effects: jitter at rest, false or lost touches near the mains-powered relay board, worse behaviour with a charger-like noise source. Thresholds in the same table would probably need retuning, with no documentation.

## Findings of 2026-11-27

### Configuration experiments on the development panel

Each try image changed one value, was flashed with `panel-cstfw.sh flash` and reverted to the vendor image afterwards (all flashes rc 0 on attempt 1, except one revert that was refused with rc 3 and done with `recover`). INT falling-edge interval while circling, median:

| Image | Median interval | Rate |
|---|---|---|
| vendor | 16–18 ms | 55–60 Hz |
| `cfg[0x66]` 20 → 10 | 19.4 ms | 52 Hz |
| `cfg[0xb4]` 339 → 170 | 26.4 ms | 38 Hz |
| `cfg[0xb4]` 339 → 420 | about 25 ms (from `/proc/interrupts`, about 38–40 interrupts/s; no clean capture) | about 40 Hz |

Every change made the controller slower. `cfg[0xb4]` slows it in both directions, which fits a drive frequency tuned to this sensor rather than a scan length. `cfg[0x66]` was only tried downwards. The `0xD204` row stayed `01 4b a8 00` on the 420 image, as on the vendor image.

### Frame counter (`0xD040`)

`0xD040` returns 6 bytes: a big-endian 32-bit counter that the flash main loop increments once per frame (`0x2f2`) and a check value, `b0 + b1 + b2 + b3 + 0xA5 == b4 << 8 | b5`. It is readable in normal mode without any command; `tools/perf/touchprobe` reads it with the `counter<ms>` option (`panel-touchprobe.sh ... counter200`). Measured on the vendor image (`tools/perf/captures/touch-counter-*.txt`):

| State | Frames/s | Reports (INT)/s |
|---|---|---|
| no touch | 83 (12 ms), steady | 0 |
| one finger resting | about 38 (26 ms) | about 38 |
| slow circles | 48–53 | 42–44 |

The scan without a finger is fast. A frame with a finger takes about twice as long, and the controller reports on almost every touched frame. The report rate is therefore limited by the work done in touched frames, not by the scan length that the front-end fields set. The flash noise logic does not explain it (next section); the extra time lies in ROM work of frames that report, or in the host's I²C read overlapping the next scan.

### False lift-offs

The controller itself reports lifts that did not happen while a finger moves: about 0.7 per second, median 57 ms, maximum 121 ms, no ghost touches at rest. In a raw capture almost every false lift is a single read with point count 1 and status 0, with the finger pressed again in the next read; a real lift comes one read (about 20 ms) after such a read.

### Vendor Android driver

Static reading of `cst2xx_ts_worker` and the rest of the vendor driver in the Android kernel:

- A lift is reported on the first frame with no pressed point: no debounce, no timer, no count of empty frames. A point is pressed when `(b0 >> 1) & 7 == 3`.
- The interrupt is requested rising-edge and oneshot. The ESD work runs every 1.2 s (300 jiffies at 250 Hz): `0xD109`, then up to five reads of `0xD040` 4 ms apart, reset when the check fails. While it holds the shared busy flag the worker skips a frame without reading or acknowledging it.
- There is no factory test, raw-data or diff path: no sysfs, proc, ioctl or misc node. Commands sent: `0xD106` at probe and resume, `0xD105` at suspend, `0xD109` in the ESD work. `0xD101` and `0xD102` are never sent.

### Flash I²C handler and debug modes

- `0xD000–0xD050` read an 81-byte RAM buffer in any mode; other `0xD0xx` addresses return `0xA5`. A 13 × 8 node map (208 bytes) does not fit, so the flash code cannot return raw or difference data. What `0xD101`, `0xD102` and the other `0xD1xx` commands do is in ROM.
- `0xD1F0–0xD213` and `0xD220–0xD240` are served from the flash trailer in any mode; no info-mode command is needed to read them.
- Data bytes written after `0xD0xx` go to RAM (how `0xD000 0xAB` acknowledges), after `0xD1xx` to RAM byte `0x200000E1`, but after `0xD1F0` or `0xD220` to a flash address. Never write data to those two.

### Noise logic and the cost of a touched frame (static analysis, 2026-11-28)

Static reading of the vendor image `0x03000001` (checksum `0x99630e41`), Thumb disassembly checked against the binary. ROM code is not available, so whatever happens inside ROM calls is inferred from timing only. Confidence: **high** = read directly from the code, **medium** = code plus a plausible interpretation, **low** = guess.

#### Where the time goes in a frame

Order inside one main loop iteration (`0x02d8`): ROM `0xdfb8`, frame counter `0x2000000c` += 1 (`0x2f2`, read at `0xD040`), ROM `0xdfb0`, ROM `0xdfa8`, `0x025c`, (ROM `0xdfa0` only if `cfg[0x21]` ≠ 0; vendor 0), ROM `0xdf98`, ROM `0xdf90`, `0x04f2`, ROM `0xdf88`, `0x068a`, **INT high** (`0x368`), `0x09fc`, ROM `0xdf80`, `0xdf68`, `0xdf60` (argument: the address of `0x068a`), `0xdf58`, `0x12e0`, `0x096c`, `0x0568` report (**INT low**), `0x0470` wait. (high)

So the INT line splits a frame in two: INT high → INT low covers the processing and the report, INT low → INT high covers the wait, the next scan and the noise statistics.

Measured in `tools/perf/captures/touch-counter-finger-2026-11-27.txt`: INT high → low median 0.69 ms (p10 0.54, p90 0.92), INT low → high median 18.8 ms (p10 13.4, p90 35). A linear fit over the 200 ms counter windows while circling gives about 20.5 ms for a frame that ends in a report and 13.5 ms for a frame without one (idle frames: 12 ms). The extra time therefore sits between INT low and INT high (wait, ROM scan `0xdfb8…0xdf88`, `0x025c`, `0x04f2`, `0x068a`), and it goes with frames that report, not with the finger as such. (fit: medium)

The flash code in that window is a few hundred µs at most (104-node loops of about 10–30 cycles per node) and does not depend on a touch, except for the optional delay in `0x068a` below. **The 10–14 ms extra are spent in ROM calls or in the host's I²C read overlapping the next scan, not in the flash noise logic.** (medium-high)

#### Code that runs only with a touch

- `0x0568`, report path (`0x05f8`), taken when ROM state says a point is present (`0x20000a23` ≠ 0, or `0x20000a66` ≥ `cfg[0x28]` and bit 7 of `0x20000a60`) and the gesture flag `0x20000a71` is 0. It marks `0x20000064` = `0x55` (a report was sent, so a release report follows later), then (high):
  - while the palm hold-off `0x20000002` is non-zero: no report at all, except at the values 59 and 1, where it writes an empty report (`0xD000` = `0xAB`, bytes 1–5 = 0, `0xD006` = `0xAB`, point count 0) and pulses INT low with ROM `0xd516(100)`;
  - otherwise it waits for the host's acknowledge of the previous report (`0x20000010`, i.e. `0xD000`, reads `0xAB`) in a loop of at most 37500 passes (`0x064c`, constant in the literal pool at `0x0888`), then ROM `0xdf40` builds the report and INT goes low.
- The acknowledge wait is about 10 cycles per pass on a Cortex-M0, so at most about 375 000 cycles: 18.8 ms at 20 MHz, 15.6 ms at 24 MHz, 11.7 ms at 32 MHz. It lies between INT high and INT low, which is measured at 0.7 ms, so with `touchprobe` (acknowledge about 3.8 ms after INT low) and the driver it does not run. Without an acknowledge it would cost the whole timeout on every touched frame. (code: high; cycle count: medium, depends on flash wait states)
- `0x05b0`/`0x05c4`, no-point path: if `0x20000065` = `0x55`, or `0x20000064` = `0x55` and `0x20000a1d` = 6, it writes a release report (`0xD000` = 0, point count 0, `0xD006` = `0xAB`) and pulls INT low. Otherwise there is no report and no INT (the idle case). (high)

ROM delay `0xd516` is called with 100 (palm empty-report INT pulse), 40 (INT pulse in `0x09fc`) and 5000 (noise case below). Its unit is not visible. INT pulse widths of 40 and 100 and the watchdog counter being cleared right before the 5000 call fit microseconds, so 5000 is probably 5 ms (medium-low). If it were a plain busy loop counting cycles or loop passes it would be 0.2–1 ms at 20–32 MHz; milliseconds (5 s) do not fit any measurement.

#### Noise statistics

`0x025c` (every frame): for each of the 13 rows, the spread (max − min) of `8 × 0x20000884[row] − 0x200007d0[row][k]` over its 8 nodes; `0x20000001` = sum over rows of (row spread − smallest row spread). What the two arrays hold is not known (ROM fills them). This byte selects the noisy branch below. (code: high; meaning: low)

`0x04f2` (every frame after frame 10): looks at the byte map `0x2000044c` (13 × 8). If any node is 255 or any row maximum lies in 11…59, and the palm hold-off is 0, `0x20000005` += 1 (stops at 100); if no such node, `0x20000005` = 0. So `0x20000005` counts consecutive frames with a moderately raised second map. (code: high; meaning of the map: low)

`0x068a` (every frame, and its address is passed to ROM `0xdf60`, which may call it again): over the signed 16-bit difference map at `0x20000668` (13 × 8):

- `neg` = sum of nodes below −40, `pos` = sum of nodes above 40, `n30` = number of nodes above 30. Positive is touch. (high)
- Noisy branch (`0x20000001` > 25): if `neg` < −500 and |`neg`| > `pos`: set bit 0 of `0x20000ab0` (re-baseline request, see below), clear the watchdog counter `0x40000204`, **ROM `0xd516(5000)`**, `0x20000008` = 6000, return early (no row correction this frame). Else if `neg` < −200 and |`neg`| > `0x20000008`: `0x20000008` = |`neg`|. (high)
- Quiet branch (`0x20000001` ≤ 25): if |`neg`| > `pos` and `neg` < −400: `0x20000008` = 6000; else the same peak update as above. No delay, no re-baseline. (high)
- Drift: if `0x20000005` > 20, palm hold-off 0, `0x20000003` = 0 and `pos` < 600: re-baseline request, `0x20000008` = 6000, `0x20000005` = 0, return early. (high)
- Palm hold-off `0x20000002`: set to 60 when `n30` > 30 (more than 30 of 104 nodes above 30), the chip is not in its low-power wait (bit 3 of `0x20000ab0`), more than 100 frames since start and the hold-off is below 10; re-armed to 60 when `n30` > 10 while it is 1…9; decremented by one per call. One finger covers far fewer than 30 nodes, so this is palm or large-object rejection. Effect in `0x0568`: an empty report on the first frame, about 58 frames without any report, another empty report at the end. (code: high; "palm": medium)
- Row common-mode correction (every frame unless returned early): for each row, `avg` = row sum / 8. If `avg` > 0: nodes ≥ `avg` and below 60 lose `avg/2` (all of `avg` if `0x20000003` ≠ 0, which the flash code never sets), nodes ≥ 60 stay, positive nodes below `avg` become 1. If `avg` ≤ 0: nodes ≤ `avg` lose `avg`, negative nodes above `avg` become −1. Any row with a node above 60 clears `0x20000a1d`. (high)

`0x20000008` is initialised to 1000 (`0x0218`) and only ever written: peak |`neg`|, or 6000 as an event mark. No flash code compares anything against it to make a decision; it is copied to `0x200008a6` in a statistics block at `0x20000890` that is outside the 81-byte I²C buffer. It is a dead statistic for the host, and the ROM is not given its address. (flash: high; ROM not using it: medium)

`0x20000001`–`0x20000008` lie below the I²C buffer (`0x20000010` = `0xD000`), so the host can neither read nor write them. (high)

Re-baseline request: bit 0 of `0x20000ab0` is also set at main loop entry. After ROM `0xdfb0`, if the bit is still set, the loop sets `0x20000065` = `0x55`, which makes the next no-point frame send a release report. Since idle frames send nothing, the ROM must clear the bit after acting on it. (medium)

`0x09fc`: a proximity-like state (counts nodes ≥ 10 among the first 34 of the map, 26 if `cfg[0x07]` ≠ 0, 20-frame hold, INT pulse with `0xd516(40)` on change). It returns at once unless `0xD04B` (`0x2000005b`) is non-zero; the init clears it and no driver writes it. Off in normal use. (high)

#### Noise path and the false lift-offs

Both reports the flash code writes by itself, the release in `0x05c4` and the palm empty report, have point count 0. The false lift-offs in the captures are point count 1 with status 0 (12 of 1818 reads in the finger capture, plus 3 with count 0), and that report is built by ROM `0xdf40`: the ROM's own tracking dropped the point for one frame. So the noise path does not write the false lifts directly. (high for the byte values, medium for the conclusion)

It can contribute indirectly through the row correction: nodes between the row average and 60 lose half the average every frame. A light or fast-moving finger whose peak is below 60 loses about a quarter of its peak (for example peak 58, row average 22 → 47), which can push it under the ROM's touch threshold for one frame. This fits lifts that happen mid-drag and never at rest with a firm finger, but the ROM threshold is unknown. (low-medium)

#### Configuration bytes

None of the noise thresholds or delays come from the configuration table. ±40, 30, −200, −400, −500, 25, 20, 600, 60, 10, 100, 1000 are instruction immediates; 5000 (`0x08a0`), 6000 (`0x08a4`) and 37500 (`0x0888`) are code literal-pool words. (high)

Configuration bytes read on these paths (file offset = `0x1604` + offset):

| Byte | File offset | Vendor value | Use |
|---|---|---|---|
| `cfg[0x07]` | `0x160b` | 0 | `0x09fc` node count (34, or 26 when non-zero) |
| `cfg[0x0b]` | `0x160f` | 6 | `0x096c` loop count (gesture path) |
| `cfg[0x21]` | `0x1625` | 0 | bit field: low part enables ROM `0xdfa0`; high nibble ≠ 0 makes frames whose `0x20000a1e` ≠ `cfg[0x6b]` skip processing and only report |
| `cfg[0x26]` | `0x162a` | 2 | INT GPIO bit |
| `cfg[0x28]` | `0x162c` | 6 | report gate in `0x0568`, compared with `0x20000a66` |
| `cfg[0x6a]` | `0x166e` | 12 | copied to `0x20000a1e` in the gesture path |
| `cfg[0x6b]` | `0x166f` | 9 | compared with `0x20000a1e` (only when `cfg[0x21]` high nibble ≠ 0) |

#### Experiments

No configuration byte changes the noise path, and nothing in it explains the 10–14 ms, so the next steps measure instead of flash. Both use existing `touchprobe` options under the panel lock, with no firmware change:

1. No finger, `touchprobe ... poll12 ack counter200`, 20 s: the host reads `0xD000` and acknowledges every 12 ms although nothing is touched. If the frame rate stays at 83/s, the extra time is ROM work in reporting frames. If it drops toward 40–50/s, the host's 3.7 ms I²C read slows down the scan (for example a scan that is restarted or extended when the I²C slave interrupt fires). In that case the next step is host-side: `clock-frequency = <400000>` on `&i2c1` (the touch controller is its only device) and/or reading 7 bytes per single-point frame instead of 28.
2. Finger resting, `touchprobe ... level noack counter200`: the acknowledge wait at `0x064c` should then run to its timeout in every touched frame. Expected: INT high → low grows from 0.7 ms to 375 000 cycles / f_CPU (12–19 ms) and frames/s drop from about 38 to about 23. The increase gives the CPU clock, which also fixes the unit of `0xd516`. If INT high → low stays short, the ROM report builder or the I²C handler acknowledges by itself and the reading of `0x064c` is wrong.

A code patch would be needed to change the row correction (`cmp r1, #60` at `0x08ac`, and the −40/40 limits at `0x069c`/`0x06b8`). Do not try it before experiment 1, and only through the review rule for risky changes: weaker noise suppression gives jitter and ghost touches near mains wiring, which a short bench test does not show.

### Sister-chip documents

- CST3240 Application Manual: same protocol (`0xD000` 7-byte reads, `0xD000 0xAB` acknowledge, status `0x06` pressed and `0x00` released, `0xD101` info mode, `0xD109` normal mode, info rows `0xD1F4–0xD20C`, same boot loader sequence). The chip has "noise listening" and "frequency hopping". Nothing on report rate, front-end parameters or debug data modes.
- CST340 datasheet V2.3: typical refresh rate 100 Hz; dynamic mode while touched, a slower monitor mode after a timeout without touch; TX drive up to 350 kHz and about 10 V. Its register map (`0x814E…`) is a different protocol from ours. Nothing on how to change the rate.
- CST226SE datasheet V1.01 exists (Chinese, on Scribd) but only its title page is public. A third-party Hynitron driver guide lists `0xD1` commands `01` debug info, `02` factory test, `03` gesture, `09` normal, `0A` sleep, `0B` software reset (unverified).

## Live chip read

Not done. The procedure requires unbinding the driver, and at the time another experiment was running on the panel with `/tmp/wallview-touch` holding `/dev/input/event1` (the Hynitron device); `wallview-touch` exits on `POLLHUP`, so unbinding would have killed a process this investigation must not touch.

What is already known from `docs/test-log.md`: the chip reports check code `0xcaca…`, version `0x03000001` and 480 × 480, i.e. the same version as the extracted image. Because the vendor kernel re-flashes on every boot when the checksum differs and the chip version is not higher, a chip with version `0x03000001` that ran this kernel must also have checksum `0x99630e41`. This is an inference; confirm it before any flashing.

To confirm (read-only, same accesses as the mainline probe): build the reader below in the VM, copy it, and run it with the driver unbound and nothing else using the touch device.

```c
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define ADDR 0x5a

static int fd;

static int xfer(struct i2c_msg *m, int n)
{
    struct i2c_rdwr_ioctl_data d = { m, n };
    return ioctl(fd, I2C_RDWR, &d) == n ? 0 : -1;
}

static int wr2(uint8_t a, uint8_t b)
{
    uint8_t c[2] = { a, b };
    struct i2c_msg m = { ADDR, 0, 2, c };
    return xfer(&m, 1);
}

static int rd(uint16_t reg, uint8_t *buf, int len)
{
    uint8_t r[2] = { reg >> 8, reg & 0xff };
    struct i2c_msg m[2] = { { ADDR, 0, 2, r }, { ADDR, I2C_M_RD, len, buf } };
    return xfer(m, 2);
}

int main(void)
{
    static const uint16_t regs[] = { 0xd1f4, 0xd1f8, 0xd1fc, 0xd200, 0xd204, 0xd208, 0xd20c };
    int rc = 0;

    fd = open("/dev/i2c-1", O_RDWR);
    if (fd < 0 || wr2(0xd1, 0x01))
        return 1;
    usleep(10000);
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint8_t b[4] = { 0 };
        if (rd(regs[i], b, 4)) {
            printf("%04x ERR\n", regs[i]);
            rc = 1;
            continue;
        }
        printf("%04x %02x %02x %02x %02x\n", regs[i], b[0], b[1], b[2], b[3]);
    }
    if (wr2(0xd1, 0x09))
        rc = 1;
    return rc;
}
```

```
limactl shell wallview -- ~/build/br/host/bin/aarch64-none-linux-gnu-gcc -O2 -o /tmp/cstinfo cstinfo.c
ssh root@PANEL 'cat > /tmp/cstinfo && chmod +x /tmp/cstinfo' < cstinfo
ssh root@PANEL 'echo 1-005a > /sys/bus/i2c/drivers/Hynitron-TS/unbind; /tmp/cstinfo; echo 1-005a > /sys/bus/i2c/drivers/Hynitron-TS/bind; ls /sys/bus/i2c/drivers/Hynitron-TS/1-005a'
```

Expected: `d1f4 0d 00 08 00`, `d1f8 e0 01 e0 01`, `d1fc dc 5f ca ca`, `d200 02 03 00 00`, `d204 01 4b a8 00`, `d208 01 00 00 03`, `d20c 41 0e 63 99`. Any difference in `d208`/`d20c` means the chip does not hold the extracted image and plan (a) below is no longer a no-op.

## Plans (not executed)

Ordered from least to most risk. Use the development panel only; the second panel stays untouched as the reference.

### 0. Without flashing

1. Confirm version and checksum with the reader above. Risk: none beyond a brief loss of touch.
2. Keep improving resampling in `wallview-touch`. This is what phones do with the same class of controller.
3. Optional, low to medium risk: with the driver unbound, send single `0xD1xx` commands (`0xD100–0xD120` except `01`, `05`, `09`, `0E`) and measure the INT period with `tools/perf/touchprobe`. State is in RAM and a reset pulse restores it, but it cannot be excluded that some command stores something in flash. Only worth it after step (a) has proven recovery.

### (a) No-op re-flash of the original image, to prove the procedure and recovery

1. Preconditions: step 0.1 shows `0x03000001` / `0x99630e41`; image sha256 matches; panel on stable power; driver unbound; no other process on `/dev/i2c-1` address 0x5a; reset line (gpio0 line 12) requested as a line handle, INT line not requested as an event.
2. Write a userspace flasher that follows the table in "Vendor update protocol" exactly, including delays. First build it with a dry-run mode that only does: reset, `A0 01 AA`, read `0xA003`, then reset again. Risk: low. No flash command is sent; a reset returns the chip to the application. Expected `0x55`. This alone demonstrates that boot mode is reachable from reset without the application's help.
3. Still without programming: in boot mode read `0xA000` and `0xA008`. Unknown whether the bootloader reports a checksum before any programming; if it returns `0x99630e41`, verification is proven before anything is erased. Risk: low.
4. Full run with the unmodified image: clear key, 15 pages, checksum compare, exit, reset. Risk: medium. From the first `A0 04 EE` until the last page the application is invalid. If anything fails, do not improvise: reset and run the same procedure again from page 0, as the vendor driver does (5 attempts). Do not power-cycle in the middle if avoidable.
5. Verify: reader shows the same seven values, driver binds, touch works, `touchprobe` timing unchanged.
6. Fallback if the userspace flasher cannot get the chip back: boot the vendor Android kernel (`extract/boot.img`). Its driver re-flashes at probe whenever `0xD20C` does not read `0x99630e41`, provided the chip still acknowledges a 2-byte write at 0x5a after reset. If the chip does not acknowledge at all in that state, the vendor driver gives up ("test iic error") and only our own flasher remains, so step 2 must have succeeded before step 4 is attempted.

Worst case: the boot code is not what the evidence says and the chip stays dead. Then the panel has no touch and the fix is a replacement controller or board.

### (b) Modified configuration for a higher report rate

Only after (a) has succeeded at least twice.

1. Change one field per image, starting with the front-end fields in the table above (for example `cfg[0x66]` 20 → 10, then `cfg[0xb4]` 339 → 170), recompute the checksum word, keep version `0x03000001`. Keep every image and its measurements under `local/firmware/hynitron/`.
2. Before flashing a try image: stop the kiosk browser or show a harmless page (ghost touches must not reach Home Assistant), check with `cstimg.py show` and `cmp -l` that only the changed byte and the checksum word differ, post `Start` in issue #7 with the try image's checksum. Measure each image with `tools/perf/panel-touchprobe.sh root@PANEL <seconds> <name>` (lock, marker check, unbind, trapped re-bind; output in `local/touchprobe/`): finger circling, finger still (position spread) and no finger (reads with touch are ghosts). Revert to the vendor image in the same sitting and finish with `info` and `boot-checksum`. Keep a count of flash cycles in `local/`.
3. Flash with the proven flasher, then measure: INT period and jitter with `touchprobe` (finger circling), rest jitter with a finger held still, five-finger test, palm, edge taps, behaviour with the relay board switching and with USB power versus mains.
4. Revert to the original image between experiments whenever the result is unclear.

Results:

| 2026-11-27, dev panel | vendor | `cfg[0x66]` 20 → 10 (checksum 0x99590e41) |
|---|---|---|
| no finger, 10 s: INT / ghost reads | 0 / 0 | 0 / 0 |
| finger still: INT interval median / p90 / max | 22.5 / 39.4 / 58 ms | 16.4 / 23.3 / 43 ms |
| finger still: position spread | 1 px | 1–2 px |
| circling: INT interval median / p90 / max | 14.8 / 39.6 / 118 ms (15 s) | 19.4 / 45.4 / 100 ms (10 s) |
| circling: new position median / p90 | 18.5 / 48.3 ms | 20.5 / 51.1 ms |
| circling: reads without touch | 7 of 760 | 2 of 226 |

`cfg[0x66]` = 10 speeds up reporting with a finger at rest but not while it moves, and the slow tail (p90 40–50 ms) stays. It is not the active scan period. The circling capture on the try image was short and its speed not controlled, so that row is weak. Both flashes rc 0 on the first attempt; the vendor image is back and verified. Next candidates: `cfg[0xb4]` (u16 339) and the fields at 0x1668/0x1669.

Measuring with a person at the panel works best as one capture with a fixed script read out beforehand (for example 40 s: 0–10 s no touch, 10–20 s finger still, 20–40 s circling), started by the person and split by time afterwards. Step-by-step prompts over chat arrive late and mix up the captures.

Risks, highest first:

- Wrong guess about a field makes the front end not scan at all or report ghosts. Recoverable by flashing the original, as long as (a) holds.
- A value that leaves the chip stuck early in the application (for example a watchdog loop) is still recoverable for the same reason, since boot mode is entered before the application starts.
- A rate gain bought with noise: touch looks fine on the bench and misbehaves on the wall next to mains wiring. This cannot be seen in a short test.
- A custom image must never get into a build that also runs the vendor kernel: the vendor driver would flash its own image back at boot (harmless, but confusing). The mainline driver never flashes.
- Outcome unknown: the scan may be limited by something in ROM that no config byte changes, in which case all of this buys nothing.

## References

- Mainline driver: `drivers/input/touchscreen/hynitron_cstxxx.c`, our patch `kernel/patches/0002-input-hynitron-cstxxx-add-cst226se.patch`
- Vendor functions (addresses in the vendor `Image`): `cst2xx_check_code` `0xffffff80086b42f0`, `hard_reset_chip` `0xffffff80086b39fc`, `cst3xx_esd_check_func` `0xffffff80086b3f34`, `hyn_ts_probe` `0xffffff80086b4a10`
- Measurements: `docs/performance.md` ("Touch controller report rate"), `docs/test-log.md`
- Not obtained: Hynitron CST328 / CST340 / CST3240 datasheets and application manual (buydisplay.com refuses automated download)

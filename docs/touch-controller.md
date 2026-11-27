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

- No timer in the flash code sets a frame period. The loop waits for a tick only at the end of a frame, and intervals are not multiples of anything, so the loop is free-running and the period is scan time plus processing. The irregular long gaps fit the noise logic in `0x068a`/`0x09fc` (counters at `0x20000002`, `0x20000005`, threshold `0x20000008` switching between 1000 and 6000, a ROM delay call with argument 5000 when a noise condition is met) and whatever re-scanning the ROM does.
- The scan itself is ROM code driven by the configuration table. The flash code copies these fields into the analog front-end registers at `0x40001080` (function `0x0180`):

| Config offset (file offset) | Value | Goes to |
|---|---|---|
| `0x64` (`0x1668`) | `0x06` | `0x40001088` low byte |
| `0x66` (`0x166a`) | `0x14` (20) | `0x40001088` high byte |
| `0x65` (`0x1669`) | `0x04` | `0x4000108a` low byte |
| `0x9c` (`0x16a0`) | `0x00` | `0x4000108a` high byte; when non-zero `0x40001082` = `0x122` instead of `0x102` |
| `0xb4` (`0x16b8`) | `0x0153` (339) | `0x40001086` |
| `0x67`, `0x68` (`0x166b`, `0x166c`) | `0x0a`, `0x0c` | `0x4000106e` |

These are the prime suspects for drive pulse count, integration time and clock divider per TX line (13 lines; 14 ms per scan is about 1.1 ms per line). Which is which is **not known**; there is no public register description for this front end, and the Hynitron datasheets that might help (CST328, CST340, CST3240 on buydisplay.com) could not be fetched (HTTP 403). The rest of the table is consumed by the ROM.

- No runtime switch: the vendor driver, mainline and the flash code contain no "report rate", "game mode" or "high rate" command. If one exists it is one of the undocumented `0xD100–0xD120` commands inside the ROM.

What 120 Hz would take and cost: the frame has to drop from about 14–18 ms to 8.3 ms, so the scan must be roughly halved. In any mutual-capacitance front end that means fewer drive pulses or shorter integration per line, and signal-to-noise falls roughly with the square root of the integration time (halving it costs about 3 dB), on a panel whose firmware already spends effort on noise handling. Expected side effects: jitter at rest, false or lost touches near the mains-powered relay board, worse behaviour with a charger-like noise source. Thresholds in the same table would probably need retuning, with no documentation.

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

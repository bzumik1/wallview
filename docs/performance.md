# Kiosk performance

Goal: the unmodified Home Assistant dashboard at a locked 60 FPS on the NSPanel Pro 86 (PX30). Only the panel may change (kernel, engine, build, launcher, OS settings); the page is shown exactly as Home Assistant serves it. This file records how we measure, what we found and what was tried, so the work can be repeated.

## Test setup

- Test Home Assistant: 1888 entities, about 6.7 websocket messages per second (mostly power meters of ESPHome switches updating every second), a dashboard of sections views: overview (badges, map card, history graphs, picture-entity card of an unavailable camera that shows a spinner forever), lights, climate (33 cards incl. history graphs), presence, battery, media, settings.
- Test browser on the panel without the kiosk script: `/tmp/kt.sh` (recreated after every reboot, see "Tools") starts Cog with `WEBKIT_INSPECTOR_HTTP_SERVER=0.0.0.0:9222`, settings from `/tmp/kt.env` (`SCALE`, `RENDERER`, `URL`, `COG_EXTRA`). It waits until the old Cog has exited; starting while the old one still holds DRM master gives `Cannot set mode (Permission denied)` and no frames at all.
- Each browser that pairs gets a new token and the integration revokes the panel's previous one, so switching between test browsers needs "Sign in again" (ESPHome button) and the new code from `/data/wallview/pairing` in the start URL.

## Tools

In `tools/perf/`, run on the Mac with Node 26 (global WebSocket):

| Tool | Purpose |
|---|---|
| `wkbench.mjs host:port "<async js function>"` | evaluate a function in the page through the WebKit remote inspector (`/socket/1/1/WebPage`, messages wrapped in `Target.sendMessageToTarget`) and print its JSON result |
| `wkprofile.mjs` | Timeline recording while a function runs; needs `Page.enable` first, otherwise all timestamps are 0 |
| `wksample.mjs host:port seconds` | ScriptProfiler sampling, top JS functions |
| `wkshot.mjs host:port out.png` | screenshot through `Page.snapshotRect` (the framebuffer `/dev/fb0` only shows the console under Cog) |
| `layers.mjs` | LayerTree: number of compositing layers and their reasons |
| `cdpeval.mjs port "<async js function>"`, `cdpscroll.mjs` | the same for Chromium over the DevTools protocol (port forwarded with `ssh -L`) |
| page probes `idle.js`, `scroll.js`, `allviews.js` | rAF frame intervals at rest; programmatic `scrollTop` per frame; every view in turn |
| `perf` (built from the kernel tree, static) | `perf record -e cpu-clock -F 499 -g -t <tid>` or `-a`; symbols from the CI `webkit-symbols` artifact, symbolised with `nm` (`tools/perf/symbolize.py`) because `perf report --symfs` does not use the library (it has no build ID) |

| `flipmon.c` | DRM client on `/dev/dri/card0` (no master needed, `DRM_CLIENT_CAP_UNIVERSAL_PLANES`): waits for every vblank and checks whether the primary plane shows a new framebuffer; prints flips per run and a histogram of gaps between new frames. This is what reaches the display, independent of the engine |
| `touchplay.c` | virtual multitouch screen through uinput (kernel `CONFIG_INPUT_UINPUT=y`), replays swipes (`scroll`) or taps (`tap`) the same way every time |

Build both with the Buildroot cross compiler, e.g. `host/bin/aarch64-none-linux-gnu-gcc -O2 -o flipmon tools/perf/flipmon.c $(pkg-config --cflags --libs libdrm)` with `PKG_CONFIG_SYSROOT_DIR`/`PKG_CONFIG_LIBDIR` pointing to the staging directory, and copy them to `/tmp` on the panel. Check: during programmatic scrolling on the climate view WebKit/Cog put 64 new frames on the display in 4 s (16 fps) and never two in consecutive vblanks, matching the in-page measurement.

`allviews.js` navigates with `history.pushState` + `location-changed`, waits 7 s, measures 4 s at rest and 4 s of programmatic scrolling. In-page rAF measures main-thread frames, not what reaches the display; compositor-only scrolling has to be measured separately (planned: DRM page-flip counter and uinput touch replay).

## Hardware and OS facts (verified)

- CPU 4× Cortex-A35, 256 KB L2, DRAM random latency 230–290 ns, memcpy about 0.9 GB/s; CoreMark 3255/s at 1.296 GHz, 3792/s at 1.512 GHz.
- CPU 1.416 GHz is the highest stable point on the development unit (1.512 GHz: CoreMark CRC errors in 2 of 24 runs), performance governor. DDR 664 MHz (vendor maximum). GPU Mali-G31 at 480 MHz only (lower OPPs rejected by the regulator). Temperatures about 50 °C under load.
- JSC JIT is active (DFG/FTL, concurrent JIT): a JS loop takes 0.7 s with JIT, 23 s without.
- Free RAM does not help: the work is single-thread CPU bound, the rest of the RAM is page cache.

## Results, unmodified page, scale 1 (2026-11-27)

| View | WebKit 2.50 (Cog) at rest / scrolling | Chromium 156 (Debian, cage) at rest / scrolling |
|---|---|---|
| overview | 20 / 8 fps | 40 / 22 fps |
| lights | 49 / 23 | 46 / 47 |
| climate | 13 / 11 | 39 / 37 |
| presence | 46 / 19 | 46 / 41 |
| battery | 52 / 28 | 54 / 51 |
| media | 58 / 31 | 55 / 52 |
| settings | 48 / 27 | 51 / 38 |

Chromium's window is 500×480 (Chromium's minimum window width is 500 px), i.e. 4 % more pixels. With `--force-device-scale-factor=1.5` under cage it rendered only a 320×320 window, so those first numbers were discarded.

## Real touch scrolling (display frames)

`tools/perf/touchtest.py root@<panel> scroll` replays 8 swipes (280 px in 250 ms, alternating up and down) through `touchplay` while `flipmon` records the time of every new frame on the display, and reports per swipe: frames per second from touch-down until 300 ms after release, worst gap, missed vblanks and the time from touch-down to the first new frame. Views measured 30 s after navigation, unmodified page, scale 1:

| View | Cog/libwpe (WebKit 2.50, before patches) | Chromium 156 (Debian, cage, GPU raster via ANGLE on Panfrost) |
|---|---|---|
| climate | 5–18 fps, gaps up to 216 ms, first frame 150–240 ms | 36–52 fps (mostly ~50), 0–3 missed per swipe, first frame 70–110 ms |
| lights | 11–29 fps, first frame 83–176 ms | 36–43 fps, first frame 65–111 ms |
| overview | 27–30 fps (the infinite spinner flips at 30 fps all the time) | not valid: the swipe line crosses the map card, which pans the map instead of the page |

`motionFps` (added later) counts frames between the first and the last new frame of a swipe, without the 70–200 ms before the browser starts scrolling; with it Chromium reaches 48–60 fps on climate (some swipes with no missed frame) and 42–57 fps on lights. `--enable-zero-copy --num-raster-threads=4` made no measurable difference; `SystemInfo.getInfo` shows GPU compositing and forced GPU rasterization on Panfrost through ANGLE.

### wallview-browser (WPEPlatform DRM, patches 0001–0003, `-mno-outline-atomics`), 2026-11-27

Same test, CI run 45922728022. The image lacked xkeyboard-config, so the browser crashed at start (`xkbcommon: failed to add default include path`); measured with the data copied to `/tmp/X11/xkb` and `XKB_CONFIG_ROOT`, the next image installs it.

| View | motionFps per swipe | missed vblanks per swipe | first frame |
|---|---|---|---|
| climate | 45–56 | 2–7 | 34–132 ms |
| lights | 47–58 | 1–6 | 46–90 ms |
| presence | 42–60 | 0–8 | 41–100 ms |
| battery | 44–58 | 1–8 | 49–81 ms |
| media | 42–60 | 0–8 | 41–92 ms |
| settings | 35–56 | 2–8 | 5–125 ms |

Control pages served over HTTP from another machine (`file://` cannot be opened from the HTTPS dashboard):

- static page of 200 coloured blocks, no script: 60.1 fps on all 8 swipes, 0 missed, first frame 36–52 ms;
- a compositor-only CSS `transform` animation: 299 new frames in 300 vblanks;
- the static page plus a 40 ms busy loop on the main thread every 150 ms: 50–60 fps, 2–5 missed per swipe.

So input, the scrolling thread, the compositor and DRM can hold 60 fps; the remaining missed frames come from scroll frames still waiting on the WebProcess main thread. During swipes on the settings view the main thread used 48 % of a core, the compositor 12 %, the Skia GPU worker 13 %, the scrolling thread 1 %.

### Touch controller report rate (2026-11-27)

The test replay sends a move every 8 ms; the real CST226SE does much less. Raw `/dev/input/event0` recorded while dragging a finger (`timeout 30 cat /dev/input/event0`, 24-byte `input_event` records, intervals between `SYN_REPORT` while touching): median 21.6 ms (46 Hz), 10th percentile 14 ms, 90th percentile 47.5 ms, worst 93 ms, with no fixed period. Interrupts of the `Hynitron Touch Int` line (`/proc/interrupts`) during continuous circling: 14–44 per second. The driver logs no read failures. With fewer than 60 position updates per second some display frames have no new position, so real finger scrolling cannot be smooth even when the engine is. The I²C bus runs at the default 100 kHz (one report: 28-byte read plus acknowledge, about 3.5 ms) and the interrupt is edge-triggered (`IRQ_TYPE_EDGE_FALLING`, threaded, `IRQF_ONESHOT`). 

`tools/perf/touchprobe.c` reads the controller directly to separate the controller from the driver: unbind the driver (`echo 1-005a > /sys/bus/i2c/drivers/Hynitron-TS/unbind`), sample the INT line (`gpiochip0` line 13) through a GPIO line handle about 5000 times a second, read `0xD000` (28 bytes) and acknowledge (`0xD000 = 0xAB`) like the driver, then bind again; `tools/perf/touchprobe.py` summarises. Do not use a GPIO line *event* request on that line: it leaves the interrupt mapped as both-edge and the driver then fails to bind (`irq: type mismatch, failed to map hwirq-13`) until a falling-edge-only line event is requested once. Result with a finger circling continuously:

| | median | p10–p90 |
|---|---|---|
| INT falling edge interval | 18.5 ms | 13.8–35.8 ms |
| INT low time | 14.3 ms | 9.6–21.8 ms |
| new position, reading at each INT | 22.5 ms | 14.1–54.6 ms |
| new position, reading every 2 ms regardless of INT | 20.1 ms | 13.3–45.2 ms |
| I²C read + acknowledge | 3.7 ms | |

The controller itself produces new coordinates only every 13–20 ms (50–75 Hz) with irregular longer gaps; reading faster or differently does not help, the kernel driver loses nothing, and the I²C speed is irrelevant. The scan rate is set by the controller firmware; no public document or driver (mainline, vendor `hyn_cst3xx` in the original Android kernel) shows a register to change it. Phones with such controllers make scrolling smooth by resampling touch positions to the display frames (Android `InputConsumer` resampling, Chromium scroll resampling).

The controller firmware, its update protocol and what it would take to raise the scan rate are in [touch-controller.md](touch-controller.md) (parked, with the steps to resume). On the panel side `wallview-touch` (`buildroot/board/wallview/touch/`) grabs the touchscreen and feeds the browser a virtual touchscreen at a steady 120 Hz: positions interpolated from the last samples `WALLVIEW_TOUCH_LATENCY_MS` (default 8) behind real time and predicted at most `WALLVIEW_TOUCH_PREDICTION_MS` (default 12) ahead of the last sample; touch down passes through at once. The CST226SE itself reports the finger as lifted for 30–150 ms (median about 57 ms) roughly once or twice a second while a finger is dragged (raw reads with the driver unbound, 2026-11-27), which broke lines and scrolls apart. `wallview-touch` bridges these lifts:

- **When a lift is held.** The last sample is at most `WALLVIEW_TOUCH_HOLD_RECENT_MS` (default 100) old, and the speed fitted over the last three samples is at least `WALLVIEW_TOUCH_START_PX_PER_S` (default 150), or at least `WALLVIEW_TOUCH_HOLD_PX_PER_S` (default 60) once the contact has travelled `WALLVIEW_TOUCH_HOLD_TRAVEL_PX` (default 12) from where it went down. Taps, slow nudges and fingers resting after a drag release at once, so double taps and long presses are unaffected.
- **How long.** `WALLVIEW_TOUCH_LIFT_MS` (default 100; `/data/wallview/touch-lift-ms` overrides it, 0 turns bridging off).
- **Rejoin.** A new contact in the same slot rejoins the touch when it lands within `WALLVIEW_TOUCH_REJOIN_PX` (default 30) plus `WALLVIEW_TOUCH_REJOIN_SLACK` (default 0.75) times speed times elapsed time of the position predicted from the lift velocity, and not more than `REJOIN_PX` behind the last sample along the stroke. Anything else is a new touch, and the held release is sent first in its own frame.
- **Coasting.** While a lift is held, the output keeps interpolating up to the last real sample, then glides on at the lift velocity with an exponential stop, `WALLVIEW_TOUCH_COAST_MS` (default 40) time constant and at most `WALLVIEW_TOUCH_COAST_PX` (default 40) px, only above `WALLVIEW_TOUCH_COAST_PX_PER_S` (default 150). On rejoin the difference to the real path decays by 0.7 per tick instead of jumping. A real lift releases at the coasted position, with no step back.
- **Prediction.** Velocity is a least-squares fit over the last three samples (each gap at most `WALLVIEW_TOUCH_MAX_GAP_MS`, default 60). Prediction is capped at the smaller of `WALLVIEW_TOUCH_PREDICTION_MS` (default 12) and the last sample interval, and fades out over four ticks once no sample came for 1.5 intervals, so a stopping finger does not snap back.
- **Debugging.** `WALLVIEW_TOUCH_DEBUG=1` logs every lift (age, speed, last three `ABS_MT_TOUCH_MAJOR` values, hold or release), every rejoin or reject (distance from the prediction, limit) and every expiry to `/tmp/wallview-touch.log`. That log is what the lift hold and the rejoin radius should be set from.
- **Trying settings.** Any of these variables can go into `/data/wallview/touch.env` (shell syntax, `NAME=value` per line); `/etc/init.d/S35wallview-touch restart` applies it, and deleting the file goes back to the defaults.
- **Offline test.** `cc -I buildroot/board/wallview/touch -o /tmp/wt-test buildroot/board/wallview/touch/wallview-touch-test.c -lm && /tmp/wt-test` replays synthetic strokes (bridged drag, circle near its origin, 3000 px/s flick, real flick, second finger beside a slow drag, touch behind a stroke, taps, rest then lift, slow nudge) and checks downs, ups, steps and stalls.

Neither engine holds 60 fps yet; Chromium is close because touch scrolling runs on its compositor thread with GPU raster. During Chromium swipes the GPU is about 3 % busy and the CPU 30 %, so the remaining missed frames are deadline/scheduling problems, not throughput. Disabling the deep idle states (`cpu-sleep` 370 µs, `cluster-sleep` 900 µs exit latency) made no measurable difference (lights 46–57 fps).

## Findings

- Rendering backend is not the limit: GPU vs CPU Skia raster, device scale 1 vs 1.5, Cog `renderer=modeset` vs `gles` all gave the same 7–8 fps of programmatic scrolling on the overview.
- During WebKit scrolling the WebProcess main thread uses about 91 % of one core, while JIT workers, Skia workers, the compositor and Cog use a few percent and 60 % of the CPU is idle. The Timeline only accounts for a third of that time.
- Symbolised main-thread profile (climate, scrolling): JSC property-table work on large objects (`PropertyTable::find/rehash`, `putDirectInternal`, `objectAssignFast`, `Object.values`, megamorphic get/put slow paths) and per-scroll layer geometry (`RenderLayer::convertToLayerCoords`, `updateLayerPosition`, `calculateLayerBounds`, `RenderLayerCompositor::computeCompositingRequirements`). Raw data: `out/debug/wk-climate-main*.txt` (not in git).
- Custom properties: Home Assistant defines 644 on `:root`; a root custom-property change costs 826 ms, about 45 % of it in `CustomPropertyData::operator==` (fix: `buildroot/patches/wpewebkit/0001-…`).
- Input: with Cog (libwpe) every touch goes to the page's main thread before a click or scroll starts, and there is no kinetic scrolling. WPEPlatform recognises gestures in the UI process and scrolls on the scrolling thread (`wallview-browser`).
- An infinite `ha-spinner` (unavailable camera) costs about 14 ms of every frame in WebKit because its stroke-dash animation repaints on the main thread.

## WebKit patches and build flags

`buildroot/patches/wpewebkit/` (applied through `BR2_GLOBAL_PATCH_DIR`, part of the CI cache key):

- `0001`: pointer fast path for custom property comparisons (826 ms root custom-property change, 45 % in deep comparisons).
- `0002`: no hit test on every touch move when the touch pointer is captured (implicit capture at pointerdown); the hit test was about 20 % of the main thread during a drag.
- `0003`: JSC `objectCloneFast` also clones cacheable dictionary objects. `Object.assign({}, 1888 entity states)`, which Home Assistant's websocket library runs for every state update, 2.15 → 0.59 ms. Differential test (`clone-diff.js` style: 3888 cases with deletes, symbols, getters, indexed, read-only properties, other prototypes, non-extensible objects) identical with JIT, without JIT, with continuous GC and with early optimisation, using `jsc` shells built from the same tree with and without the patch.
- `external.mk`: WebKit only is built with `-mno-outline-atomics` (Cortex-A35 has no LSE; outlined atomics cost 22.4 ns per ref/deref vs 11.7 ns inline, about 2–3.5 % of the main thread).

Measured on the panel with real touch drags (expert analysis, Cog/libwpe): the display got 18.7 new frames/s with stalls up to 1.27 s; the main thread was 93 % busy, 36 % hit testing (touch moves and wheel events, the latter because Home Assistant has a passive `wheel` listener on `document`), 36 % Home Assistant's websocket handling. Programmatic `scrollTop` scrolling (the in-page probe) takes a more expensive path (`updateCompositingLayers(OnScroll)`, 28 %) that touch scrolling does not, so it overstated the gap to Chromium. The climate view's 13 fps was a cold start; in steady state WebKit gives 40–50 fps there. The main thread runs at an IPC of 0.14 and is memory bound (DRAM miss about 340 cycles), so `-O3` is not expected to help; less memory traffic does.

## Tried and dropped

- Batching/merging Home Assistant websocket messages and overriding `ha-spinner` in an injected script (overview at rest 6 → 52 fps): removed, because it changes how the page behaves.
- 1.512 GHz CPU: unstable on this unit.

## Next steps

1. WebKit with WPEPlatform + custom-property patch + `wallview-browser` (CI build running), then measure real touch scrolling.
2. Expert analysis of the JSC and RenderLayer hot spots (flags, patches, build options).
3. Measurement of what reaches the display (DRM page flips) and touch replay through uinput (kernel option needed).
4. If Chromium stays clearly better: it needs a larger root file system than the 256 MiB slot, i.e. repartitioning the eMMC (owner's decision).

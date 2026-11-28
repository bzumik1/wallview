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

The controller itself interrupts about every 20 ms but changes the coordinates less often: in the 2026-11-27 capture about half of the coordinate changes are 15–30 ms apart, most of the rest 40–75 ms, with a tail beyond 100 ms; reading faster or differently does not help, the kernel driver loses nothing, and the I²C speed is irrelevant. The scan rate is set by the controller firmware; no public document or driver (mainline, vendor `hyn_cst3xx` in the original Android kernel) shows a register to change it. Phones with such controllers make scrolling smooth by resampling touch positions to the display frames (Android `InputConsumer` resampling, Chromium scroll resampling).

The controller firmware, its update protocol and what it would take to raise the scan rate are in [touch-controller.md](touch-controller.md) (parked, with the steps to resume). On the panel side `wallview-touch` (`buildroot/board/wallview/touch/`) grabs the touchscreen and feeds the browser a virtual touchscreen at a steady 120 Hz: positions interpolated from the last samples `WALLVIEW_TOUCH_LATENCY_MS` (default 16) behind real time and predicted at most `WALLVIEW_TOUCH_PREDICTION_MS` (default 30) ahead of the last sample; touch down passes through at once. The CST226SE itself reports the finger as lifted for 30–150 ms (median about 57 ms) roughly once or twice a second while a finger is dragged (raw reads with the driver unbound, 2026-11-27), which broke lines and scrolls apart. `wallview-touch` bridges these lifts:

- **When a lift is held.** The last sample is at most `WALLVIEW_TOUCH_HOLD_RECENT_MS` (default 100) old, and the speed fitted over the last three samples is at least `WALLVIEW_TOUCH_START_PX_PER_S` (default 150) after `WALLVIEW_TOUCH_START_TRAVEL_PX` (default 8) of travel, so a tap whose centroid drifts at lift is not held, or at least `WALLVIEW_TOUCH_HOLD_PX_PER_S` (default 60) once the contact has travelled `WALLVIEW_TOUCH_HOLD_TRAVEL_PX` (default 12) from where it went down. Taps, slow nudges and fingers resting after a drag release at once, so double taps and long presses are unaffected.
- **How long.** `WALLVIEW_TOUCH_LIFT_MS` (default 80; `/data/wallview/touch-lift-ms` overrides it, 0 turns bridging off).
- **Rejoin.** A new contact in the same slot rejoins the touch when it lands within `WALLVIEW_TOUCH_REJOIN_PX` (default 30) plus `WALLVIEW_TOUCH_REJOIN_SLACK` (default 0.75) times speed times elapsed time of the position predicted from the lift velocity, and not more than `REJOIN_PX` behind the last sample along the stroke. Anything else is a new touch, and the held release is sent first in its own frame.
- **Coasting.** While a lift is held, the output keeps interpolating up to the last real sample, then glides on from the position already shown at the lift velocity with an exponential stop, `WALLVIEW_TOUCH_COAST_MS` (default 250, close to the browser's kinetic scroll friction so the glide matches the fling that follows) time constant and at most `WALLVIEW_TOUCH_COAST_PX` (default 80) px, only above `WALLVIEW_TOUCH_COAST_PX_PER_S` (default 150) and only when the lift came at most `WALLVIEW_TOUCH_COAST_AFTER_MS` (default 85, because with kernel patch 0006 a real lift reaches evdev one read after the unpressed report, which in the capture is mostly 50–80 ms after the last coordinate change) after the last sample, so a finger that stopped before lifting does not jump forward. When the glide reaches `COAST_PX` or the screen edge the contact is released at once instead of waiting for the hold to expire, so a fast flick lets go after 30–50 ms. On rejoin the difference to the real path decays by `WALLVIEW_TOUCH_BLEND` (default 0.7) per tick instead of jumping. The same blend applies to a sample that comes more than `HOLD_RECENT_MS` and less than `WALLVIEW_TOUCH_GAP_BLEND_MS` (default 280) after the previous one without a lift in between: with kernel patch 0006 that is a false lift the kernel bridged, after which the controller jumps 60–100 px; a longer pause is a finger that rested, whose first move is still sent at once. A real lift releases at the coasted position, with no step back. A held lift that does not coast (too slow, or more than `COAST_AFTER_MS` after the last sample) stays where the output already is until it rejoins or expires. It used to drop to the last sample in one tick (a back-step of up to the whole prediction, 30 px at 1000 px/s), and after the first review it faded back instead, which still moved the output against the stroke just before the release and turned the browser's fling velocity negative. Lifts are held only for a contact that never shared the screen with another finger (checked after every frame, so a finger that rejoins next to a second one counts too): a pinch or a two-finger gesture releases at once, so its fingers do not glide apart after they leave (a 300 px/s pinch used to end 22 % wider than the fingers were). A lift is also not held while another finger is down, even one that landed in the same frame, and a new finger landing in another slot releases a held lift in its own frame first, so the browser never sees a gliding contact next to a new touch. `WALLVIEW_TOUCH_HOLD_MAX_PX_PER_S` (default 0, off) also releases at once above that speed.
- **Prediction.** Velocity is a least-squares fit over the last three samples (each gap at most `WALLVIEW_TOUCH_MAX_GAP_MS`, default 150). Prediction is capped at the smaller of `WALLVIEW_TOUCH_PREDICTION_MS` (default 30) and the last sample interval, and fades out over four output ticks (four timer periods, or four display frames in vblank mode) once no sample came for 1.5 intervals or `WALLVIEW_TOUCH_PREDICT_EASE_MS` (default 100), whichever is longer, so a stopping finger does not snap back. The fade starts at the latest four ticks before `MAX_GAP_MS`, so it has ended when prediction stops: before that clamp a prediction still at full length was dropped in one tick and the output stepped back.
- **No prediction for a resting finger.** Prediction starts only after the contact has travelled `WALLVIEW_TOUCH_PREDICT_TRAVEL_PX` (default 6) from where it went down, which halves tap and long-press jitter (2.8 to 1.4 px in the replay).
- **Idle cost.** With no finger the daemon has no timer and sleeps in `poll`. A finger that rests (no new coordinates for longer than `MAX_GAP_MS`, no hold, no blend) also stops the timer until the next position change (40 % fewer ticks on the capture, identical output); frames that change only `ABS_MT_TOUCH_MAJOR` do not wake it (before, each one armed the timer for one tick: 493 wakeups in 10 s of a resting finger with a width change every 20 ms). The first position change after such a rest is sent at once instead of one timer period later. The code is locked in memory (`mlockall`) so the first touch after a long idle never waits for page faults, and a failure to get `SCHED_FIFO` is logged.
- **Start and stop.** The daemon grabs the touchscreen only while no finger is down (it waits up to 30 s), because a grab during a touch leaves the browser with a contact that never lifts. After the grab it empties the stale queue and reads the kernel's slot state right after, so a touch that lands during start-up is followed from the first frame. `ABS_X`/`ABS_Y` follow the oldest contact, not the lowest slot, and `BTN_TOUCH` changes at most once per frame (one contact ending and another starting in one frame used to send 0 and 1). `S35wallview-touch` runs the daemon as a background child that its loop kills on `TERM`; `stop` then waits a second and repeats `killall` until no daemon is left, so a daemon still starting while `stop` runs cannot stay behind holding the grab. On `SIGTERM` or a lost input device it releases every contact it reports before destroying the virtual device; the signals are only unblocked inside `ppoll`, so a stop request cannot be missed while the daemon sleeps.
- **Display-locked output (issue #25, off by default).** `WALLVIEW_TOUCH_VBLANK=1` resamples once per display refresh instead of on its own 120 Hz timer, the way Android's input consumer is driven by the display: while a finger is down it queues a DRM vblank event on `WALLVIEW_TOUCH_DRM` (default `/dev/dri/card0`) and resamples `WALLVIEW_TOUCH_VBLANK_OFFSET_MS` (default 10) after each vblank, so the new position is in the browser's queue several milliseconds before it starts the next frame instead of racing its page-flip event. The frame length is measured from the vblank timestamps (it locks between about 40 and 120 Hz, seeded at 60 Hz); the lift-blend decay is scaled to it. A timer at three frames keeps output going if vblank events stop, and with the display off it falls back to the 120 Hz timer.
  - DRM master safety: opening a primary node with no master makes the opener master. The node is therefore opened only when another process already has it open (a `/proc/*/fd` scan comparing device numbers, retried with a backoff from 5 s, doubling up to 60 s; `WALLVIEW_TOUCH_DEBUG=1` logs how long it took), only if `DRM_IOCTL_VERSION` names `WALLVIEW_TOUCH_DRM_DRIVER` (default `rockchip`), and if `DRM_IOCTL_DROP_MASTER` succeeds anyway the node is closed again (logged once). A failing `DRM_IOCTL_WAIT_VBLANK` (display off) falls back to the timer and is retried once a second, not every tick, and a read error on the node switches to the timer at once instead of after the three-frame fallback. A non-master, unauthenticated fd may queue vblank events (`DRM_IOCTL_WAIT_VBLANK` has no `DRM_AUTH` flag).
  - While the fd stays open, a browser exit no longer is the last close of the node, so fbcon is not restored between browser restarts; the next browser still becomes master. Test a browser restart with the mode on before making it the default.
  - Reviewed (Fable 5.2, 2026-11-28): GO as opt-in and for a locked trial; the phase offset, the master gate, the driver check, the double-resample guard and the error handling come from that review. Way back: remove the line from `/data/wallview/touch.env` and restart `S35wallview-touch`.
  - The latency and prediction settings stay as they are; Android's 5 ms and 8 ms do not suit this controller (see "Comparison with Android touch resampling").
- **Debugging.** `WALLVIEW_TOUCH_DEBUG=1` logs every lift (age, speed, last three `ABS_MT_TOUCH_MAJOR` values, hold or release), every rejoin or reject (distance from the prediction, limit) and every expiry to `/tmp/wallview-touch.log`. That log is what the lift hold and the rejoin radius should be set from.
- **Trying settings.** Any of these variables can go into `/data/wallview/touch.env` (shell syntax, `NAME=value` per line); `/etc/init.d/S35wallview-touch restart` applies it, and deleting the file goes back to the defaults. A file that does not parse as shell keeps the daemon from starting (the loop retries every 2 s), and the browser then gets the raw touchscreen; `/data/wallview/touch-lift-ms` wins over `WALLVIEW_TOUCH_LIFT_MS` in the file.
- **Offline test.** `cc -I buildroot/board/wallview/touch -o /tmp/wt-test buildroot/board/wallview/touch/wallview-touch-test.c -lm && /tmp/wt-test` replays synthetic strokes (bridged drag, circle near its origin, 3000 px/s flick, real flick, second finger beside a slow drag, touch behind a stroke, taps, rest then lift, slow nudge, jittery double tap, lift after a pause, lift and land in one frame) and checks downs, ups, steps and stalls. `wallview-touch-replay.c` (same build line) feeds a raw `touchprobe` capture (`D` lines, driver unbound) through the daemon with the current `WALLVIEW_TOUCH_*` environment and prints output downs and ups against raw ones, path error against the raw path, largest step, jerks, stalls, release delay after the last touch sample and tap release delay; run it once per parameter set against the same capture. `REPLAY_KEEP_UNPRESSED=1` models kernel patch 0006 (unpressed reads keep the contact).

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

### Touch test pages

`tools/perf/touch-tests/` holds local pages for judging touch on the panel without Home Assistant or the network: `draw.html` (strokes with down/up markers, unbridged lifts, stalls, back-steps, release delay, plus scroll and tap modes), `list.html` (300 entity rows: stuck and jumping frames during a drag, fling distance and start delay, whether a touch catches a running fling), `nested.html` (horizontal card strips inside a vertical page: right, wrong, both or no axis), `views.html` (room views with scroll snapping: swipes that switched, did not switch or skipped), `controls.html` (Home Assistant style brightness and cover sliders, toggles, a tap / double tap / long press button, a thermostat ring and a colour wheel: jumps, twitch at release, tap to toggle latency) and `taps.html` (targets in the centre and at the edges: hit error, offset, extra downs). Every page shows frames per second and long frames while touched, and keeps results in `window.__touchLog` for reading over CDP. Serve the folder from the Mac (`python3 -m http.server` in it) and point the browser at it, or copy it to the panel and open it as `file://`.

`tools/perf/touch-tests/ha/` is the same idea with real Home Assistant cards: `wallview_touch_test.yaml` is a package of test helpers (input booleans and numbers, a template light with brightness and colour, a template cover, a generic thermostat on a template sensor), and `dashboard.yaml` is a dashboard (raw configuration editor) with tiles and their brightness, cover and temperature features, a thermostat card, a light card, a button with tap, double tap and hold actions, sliders, and a long list view. The helpers touch no real device; removing the package removes them.

### Tuning against the 2026-11-27 capture

The controller produces new coordinates mostly 15–75 ms apart and repeats the last position in between, so with 8 ms latency the output mostly waited for the next sample and then jumped. Replaying `touch-raw-2026-11-27.txt` (reference path: linear between coordinate changes):

| Setting | Output downs (raw 81) | Jerks | Stalls | Largest step | Path error p50 |
|---|---|---|---|---|---|
| latency 8, prediction 12, gap 60 (before) | 79 | 845 | 913 | 130 px | 5.4 px |
| latency 16, prediction 30, gap 150 (now) | 76 | 550 | 259 | 104 px | 7.0 px |
| same, with kernel patch 0006 | 64 (raw 65) | 659 | 291 | 104 px | 7.3 px |

Latency 24 smooths further (jerks about 380 in the offline prototype) at 8 ms more lag; that is a feel decision for a panel A/B. Offline comparisons on the same capture: a quadratic least-squares predictor (as in Chromium) and a constant-acceleration Kalman filter were both worse than linear interpolation on every metric, and a time-only debounce (as tslib's `debounce` module) bridged only 4 more lifts while delaying every tap by the debounce time, so neither replaces `wallview-touch`. After patch 0006 about 7 false lifts in 27 s of circling remain that look exactly like real lifts; holding them costs every real lift of a moving finger the hold time.

### Comparison with Android touch resampling (2026-11-28)

Read from AOSP `frameworks/native` (main): `libs/input/InputConsumer.cpp`, `libs/input/Resampler.cpp`, `services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp`. Android resamples in the app's `InputConsumer` (current AOSP: enabled unless `ro.input.resampling` is 0). The panel's original system is Android 8.1.0, build OPM8.190605.003 (AOSP android-8.1.0_r65, vendor version NSPanel86P_1.6.5). Its `libinput.so` from the backup has the same constants (5 ms latency, 2 and 20 ms sample gap limits, 8 ms prediction cap), and `ro.input.noresample`, the switch of that release, is not set in the system or vendor properties, so resampling was on.

| | Android | wallview-touch |
|---|---|---|
| Sample time | frame time − 5 ms (`RESAMPLE_LATENCY`) | tick − 16 ms (`LATENCY_MS`) |
| Interpolation | between the two samples around the sample time, skipped if they are < 2 ms apart | same, between coordinate changes |
| Extrapolation | last two samples, only if 2–20 ms apart, at most min(gap / 2, 8 ms) | least-squares velocity over up to 3 samples ≤ 150 ms apart, at most min(gap, 30 ms), faded out after 1.5 × gap |
| Lifts | passed through at once; `MultiTouchInputMapper` releases a slot in the same sync, no debounce | lifts of a moving finger held up to 80 ms and rejoined near the predicted position |
| After a gap | nothing | coasting with exponential stop, blend back onto the real path |
| New sample behind the last resampled one | replaced by the resampled position (`rewriteMessage`) | output follows the new sample at once, which can step back up to 14 px after a prediction (24 such steps of 3 px or more in the capture); only a rejoin blends the difference away per tick |

The vendor Android touch driver reports a lift on the first frame without a pressed point (see `docs/touch-controller.md`), so Android passed every false lift of this controller to apps.

Android's limits assume a controller reporting every 4–10 ms. This controller changes coordinates mostly 15–75 ms apart, so Android would almost never extrapolate (gap > 20 ms) and a 5 ms latency interpolates only a small part of each gap. Replay of `touch-raw-2026-11-27.txt` with Android-like settings (`LATENCY_MS=5 PREDICTION_MS=8 MAX_GAP_MS=20 LIFT_MS=0 COAST_MS=0`; an approximation, not a port):

| | Android-like | defaults |
|---|---|---|
| Output touch-downs (raw 81) | 81 | 77 |
| Ticks while down | 2059 | 4610 |
| Stalls | 874 | 406 |
| Jerks | 1132 | 499 |
| Max step | 146 px | 98 px |
| Path error p50 / p95 | 4.4 / 21.5 px | 6.8 / 30.6 px |
| Release after last sample p50 / max | 23 / 27 ms | 27 / 114 ms |

Numbers from the code after the fourth audit (2026-11-28), without patch 0006. Path error is measured only on ticks while the output is down; the Android-like run releases at every false lift and so leaves out the gaps, where the error of a bridged stroke is largest, which is why its path error is lower although it shows the browser 4 extra touch-downs and five times the stalls per tick down.

### wallview-touch audit (2026-11-28)

Three independent reviews (Fable 5.2: correctness with a 200,000-frame fuzzer under ASan/UBSan, latency and efficiency with callgrind, touch feel with the replay) of the code after the vblank change. Applied:

- With kernel patch 0006 the old coast settings killed flings: a real lift arrives 50–78 ms after the last coordinate change, beyond `COAST_AFTER_MS` 40, so the held contact snapped back up to 75 px and stood still for 100 ms before releasing. Modelled fling velocity (WebKit's kinetic scroll, sum of deltas over the last 150 ms, model not checked against WPE source) fell to 0.04 of the finger's. The coast now starts from the shown position, uses the defaults above and releases when spent: fling ratio 0.92 (minimum 0.69), release delay of flicks 53 ms instead of 103 ms, worst back-step at a lift 17 px instead of 75.
- `LIFT_MS` 100 to 80: the bridgeable false lifts left with the patch are 36–72 ms long; 100 bridges nothing more.
- Start of a stroke: when the second sample arrived less than the latency after touch-down, the output jumped to it and stepped back on the next tick. The output now holds the first sample until the interpolation target reaches it (test "early second sample").
- An out-of-range `ABS_MT_SLOT` no longer redirects the following events to the previous slot; option values above 10⁶ are rejected; a resync clears a stale start on a free slot.
- `EVIOCGMTSLOTS` fills only the device's 5 slots; the other 5 kept tracking id 0, so a resync after `SYN_DROPPED` started phantom contacts at 0,0 (found by the audit run on the Mac, `docs/touch-audit-mac-2026-11-28.md` on `claude/project-thread-3bjdnn`). Tracking ids are now prefilled with −1. A read error other than `EAGAIN` on the DRM node closes it instead of spinning.
- Efficiency: the timer stops for a resting finger, input and vblank reads stop at a short read, the `/proc` scan for vblank mode runs only while no finger is down (never on the touch path), and a pending phase timer is cancelled whenever the vblank mode is left.

Replay of `touch-raw-2026-11-27.txt` with `REPLAY_KEEP_UNPRESSED=1` (patch 0006, modelled at that time without its timer, so the down counts here were optimistic; see the fifth review below): 64 output downs, path error p50/p95 7.3/35.3 px, max step 104 px, jerks 613 (659 before), stalls 310 (291 before; more because of the travel threshold). Without the patch: 77 downs, jerks 543, stalls 278.

Fourth review (Fable 5.2, same three angles plus a `SYN_DROPPED` harness) of the result above. Applied:

- Resync after `SYN_DROPPED` ran inside the event handler at the first `SYN_REPORT` after the drop, while the rest of the stale queue was still unread; that queue could then re-apply an old touch-down as a new contact (a phantom tap) or lift a finger that was still down. The daemon now ignores every event after the drop, empties the queue and reads the slot state once, after the queue is empty. Harness: one finger held across the drop stays down with no extra tap; a resting second finger stays down while the first one taps during the stall.
- A repeated `ABS_MT_TRACKING_ID` with the id the slot already has (sent again after a resync) no longer restarts the contact.
- Prediction fade: `PREDICT_EASE_MS` default 0 to 100, clamped so the fade ends before `MAX_GAP_MS` (see Prediction above). Replay with patch 0006: path error p95 35.3 to 33.8 px, max step 104 to 98 px, jerks 613 to 573, stalls 310 to 437. The extra stalls are ticks where the output, already ahead of the finger by the prediction, waits for it instead of stepping back; a pause reads as smoother than a back-step, so the trade was taken. `WALLVIEW_TOUCH_PREDICT_EASE_MS=0` restores the old fade for an A/B on the panel.
- Option values from an environment file with CRLF line ends are accepted instead of ignored.
- 200,000-frame fuzzer after the changes: no invariant violations, no stuck or lost contacts. Not changed: `ABS_MT_TOUCH_MAJOR` is still sent only at touch-down, and `ABS_X`/`ABS_Y` single-touch emulation is still written.

Fifth review, the first of the review loop Jakub asked for (2026-11-28: a Fable 5.2 and an Opus 5.5 touch input reviewer each review the code independently, the findings are fixed, and the loop repeats until neither finds anything). Both reviewers fuzzed the daemon under ASan/UBSan (5 slots, 0 protocol violations in 4 million output frames) and both found, independently, the same main defect:

- Kernel patch 0006's 60 ms release timer let 7 of the 16 single-read false lifts through, as lifts too old for the daemon's hold; the replay modelled the patch without the timer, so the documented 64 output downs were really 69. The timeout is now 110 ms and the replay models the timer and the two-read limit (details in `docs/touch-controller.md`). Replay with the patch: 64 output downs, jerks 571, stalls 437, path error p95 33.8 px.
- A held lift that did not coast dropped from the predicted position to the last sample in one tick (30 px at 1000 px/s, or on every held lift with `COAST_MS=0`). It then kept the prediction fade; the sixth review replaced that with holding the shown position (see Coasting above). The "lift after pause" test passed only because of that drop; it now checks that the release happens where the output was at the lift, with no back-step.
- In vblank mode the prediction fade took two display frames instead of four, because it used the 120 Hz timer period.
- A touch landing in the microseconds between the startup grab check and the stale-queue drain was lost for the whole stroke. Startup now creates the virtual device and then resyncs from the kernel's slot state, as after `SYN_DROPPED`.
- `stop` could miss a daemon whose start script was still reading `touch.env`, which then kept the grab with no supervisor. The loop now runs the daemon as a child it knows and kills it on `TERM`; `stop` waits for the loop before the `killall` fallback.
- Kernel patch: a corrupt finger id mid-frame left a half-reported frame (see `docs/touch-controller.md`; the sixth review replaced this fix with a check of all ids before any point is reported).
- The offline test now checks that its options match `load_options()` defaults.

Kept as it is: when a new sample lands behind a predicted position, the output follows the sample at once (24 back-steps of 3 px or more in the capture, worst 14 px). Blending those away (Fable's variant) removes the back-steps but raises path error p50 from 7.1 to 9.7 px and stalls from 416 to 707, so it is a feel question for an A/B on the panel, not a fix. Fuzzers after the changes: 4 million output frames (Opus harness) and 750,000 frames (Fable harness), 0 violations; the `SYN_DROPPED` harness still passes.

Sixth review (round 2 of the loop, on e91b323, plus the items of a parallel round 1 run from the Mac, `docs/touch-review-loop.md` on `claude/project-thread-3bjdnn`). Neither reviewer found a stuck, phantom or lost contact; the protocol fuzzers, now including a vblank-mode one, ran more than 15 million output frames with no violation. Applied:

- Held lifts that do not coast hold the shown position instead of fading back against the stroke (Coasting above), and lifts are not held for a contact that shared the screen with another finger (pinch overshoot).
- `ABS_X`/`ABS_Y` follow the oldest contact; `BTN_TOUCH` is sent once per change from `flush()`.
- Width-only frames no longer wake the timer, and the first move after a rest is sent at once (Idle cost above).
- Vblank mode: retry of a failing `WAIT_VBLANK` once a second, immediate timer on a DRM read error, backoff capped at 60 s as documented, a log line when the node had no master.
- `ease_from` cannot go negative with a `MAX_GAP_MS` below four ticks.
- `S35wallview-touch`: the stop race (Start and stop above).
- Kernel patch 0006: all finger ids are checked before any point of a frame is reported, so a corrupt id never leaves a half-reported frame (the `input_mt_sync_frame()` fix of the fifth review could lift valid fingers listed after the corrupt one); each slot has its own keep deadline, so one finger's unpressed read no longer extends another's; the release work runs on `system_highpri_wq`.
- Replay: kept duplicates no longer count as touch samples, so the release delays are measured from the last pressed read. This shows the cost of patch 0006 on taps: release p50 73 ms (max 130) after the last pressed read with the patch, 24 ms (max 27) without it. That is the price of not breaking strokes, and is the first thing to judge in the panel trial.
- Tests: oldest-contact pointer, no hold in a two-finger touch, one `BTN_TOUCH` per frame, width-only frames, immediate first move, pinch lift. Each new test fails on e91b323.

Considered and not done:

- A distance gate in patch 0006 (release a kept contact that comes back far away): after a false lift the controller often jumps 60–100 px while the finger keeps moving, so a gate would split exactly the strokes the patch exists to join. A re-press with the same finger id within two reads or 110 ms of an unpressed read has not been seen in the capture.
- Extending prediction beyond `PREDICTION_MS` while a lift is pending: with patch 0006 the output stands still before 12 of 27 moving lifts (the kernel reports the lift 50–110 ms after the last coordinate change). Predicting further would overshoot every finger that really stopped; this belongs to the panel A/B with `PREDICTION_MS`.
- A new sample behind the predicted output still moves the output back at once (fifth review).
- In vblank mode a vblank requested before the last lift still wakes the daemon once after it, and the `/proc` scan runs at `SCHED_FIFO` after a lift at most once per backoff period (up to 60 s); both are rare and short.
- `S35wallview-touch` trims the log only when it restarts the daemon, so with `WALLVIEW_TOUCH_DEBUG=1` the log grows in RAM until then; debug is for short sessions.
- `CONFIG_HZ` is not set in `kernel/config`, so arm64's default 250 applies and the 110 ms timeout is 108–112 ms; at HZ 100 it would be 100–110 ms.

Seventh review (round 3 of the loop, on 1565570). No stuck, phantom or lost contact, nothing in patch 0006 or the init script; fuzzers (protocol, vblank mode, "no output down without an input down", 400 runs with random option values) ran clean under ASan/UBSan, and a 20,000-stroke simulator found no back-step in hold, coast, rejoin or release. Applied:

- The multi-touch flag was set only when a contact started, so it missed a finger that rejoined next to a second one (the pinch glide came back: output 262 px apart for fingers 220 px apart) and wrongly marked a finger that landed in the same frame another one lifted. It is now set after each frame for every contact that is down together with another.
- A held lift stayed on screen for up to 80 ms next to a new finger in another slot, which the browser could read as a two-finger gesture. Such a lift is now released in its own frame before the new finger goes down.
- Start-up drains the stale queue right before the slot resync, not before creating the virtual device, so frames from that window are not applied twice.
- Tests: second finger during a held lift, lift and land in other slots in one frame. Each fails on 1565570.

Noted, not done here: the touch I²C bus (`&i2c1`) has no `clock-frequency` in the device tree, so it runs at 100 kHz and a report read takes 3.5–3.7 ms. At 400 kHz each coordinate and lift would reach evdev about 2.5 ms sooner, if the CST226SE and the board accept it. That is a device tree change on the panel and needs its own review and trial.

Eighth review (round 4 of the loop, on 45cde76). No stuck, phantom or lost contact; nothing new in patch 0006, the init script or the build. Both reviewers found the same case, and Fable found one feel issue:

- A moving finger in a higher slot that lifted in the same frame a new finger landed in a lower slot was still held and glided for 80 ms next to the new finger (slots are processed in order, and the multi-touch flag is set only after the frame). A lift is now never held while another contact is down. A three-slot fuzzer that found about 2,500 such cases per seed finds none.
- With patch 0006 a false lift bridged in the kernel reached the daemon as a plain sample 100–250 ms after the previous one, and the controller's jump after it went to the screen in one tick (six steps of 56–98 px in the capture, none of them visible without the patch because the daemon's rejoin blends them). Such samples are now blended like a rejoin (`GAP_BLEND_MS` above). Replay with the patch: steps of 50 px or more 21 to 14, jerks 571 to 511, stalls 437 to 403, path error p50/p95 7.2/33.8 to 7.6/37.4 px. Without the patch: jerks 499 to 444, stalls 406 to 372. `WALLVIEW_TOUCH_GAP_BLEND_MS=0` turns it off for an A/B on the panel.
- Tests: moving lift with a new finger in a lower slot in one frame, jump after a bridged gap (largest step 30 px instead of 100). Both fail on 45cde76.

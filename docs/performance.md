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

The controller firmware, its update protocol and what it would take to raise the scan rate are in [touch-controller.md](touch-controller.md) (parked, with the steps to resume). On the panel side `wallview-touch` (`buildroot/board/wallview/touch/`) grabs the touchscreen and feeds the browser a virtual touchscreen at a steady 120 Hz (`WALLVIEW_TOUCH_RATE`, 2–1000, default 120): positions interpolated from the last samples `WALLVIEW_TOUCH_LATENCY_MS` (default 16) behind real time and predicted at most `WALLVIEW_TOUCH_PREDICTION_MS` (default 30) ahead of the last sample; touch down passes through at once. The CST226SE itself reports the finger as lifted for 30–150 ms (median about 57 ms) roughly once or twice a second while a finger is dragged (raw reads with the driver unbound, 2026-11-27), which broke lines and scrolls apart. `wallview-touch` bridges these lifts:

- **When a lift is held.** The last sample is at most `WALLVIEW_TOUCH_HOLD_RECENT_MS` (default 100) old, and the speed fitted over the last three samples is at least `WALLVIEW_TOUCH_START_PX_PER_S` (default 150) after `WALLVIEW_TOUCH_START_TRAVEL_PX` (default 13) of travel, so a tap whose centroid drifts at lift is not held, or at least `WALLVIEW_TOUCH_HOLD_PX_PER_S` (default 60) once the contact has travelled `WALLVIEW_TOUCH_HOLD_TRAVEL_PX` (default 13) from where it went down. Taps, slow nudges and fingers resting after a drag release at once, so double taps and long presses are unaffected.
- **How long.** `WALLVIEW_TOUCH_LIFT_MS` (default 80; `/data/wallview/touch-lift-ms` overrides it, 0 turns bridging off).
- **Rejoin.** A new contact in the same slot rejoins the touch when it lands within `WALLVIEW_TOUCH_REJOIN_PX` (default 30) plus `WALLVIEW_TOUCH_REJOIN_SLACK` (default 0.75) times speed times elapsed time of the position predicted from the lift velocity, and not more than `REJOIN_PX` behind the last sample along the stroke. Anything else is a new touch, and the held release is sent first in its own frame.
- **Coasting.** While a lift is held, the output keeps interpolating up to the last real sample, then glides on from the position already shown at the lift velocity with an exponential stop, `WALLVIEW_TOUCH_COAST_MS` (default 250, close to the browser's kinetic scroll friction so the glide matches the fling that follows) time constant and at most `WALLVIEW_TOUCH_COAST_PX` (default 80) px, only above `WALLVIEW_TOUCH_COAST_PX_PER_S` (default 150) and only when the lift came at most `WALLVIEW_TOUCH_COAST_AFTER_MS` (default 85, because with kernel patch 0006 a real lift reaches evdev one read after the unpressed report, which in the capture is mostly 50–80 ms after the last coordinate change) after the last sample, so a finger that stopped before lifting does not jump forward. When the glide reaches `COAST_PX` or the screen edge the contact is released at once instead of waiting for the hold to expire, so a fast flick lets go after 30–50 ms. On rejoin the difference to the real path decays by `WALLVIEW_TOUCH_BLEND` (default 0.7, values above 0.95 are ignored with a log line) per tick instead of jumping. The same blend applies to a sample that moves more than `REJOIN_PX` (straight-line) and comes more than `WALLVIEW_TOUCH_GAP_BLEND_MIN_MS` (default 60) and less than `WALLVIEW_TOUCH_GAP_BLEND_MS` (default 280) after the previous one without a lift in between: with kernel patch 0006 that is a false lift the kernel bridged or a missed report in a fast swipe, after which the controller jumps 60–150 px. Smaller moves, and anything after a longer pause, are sent at once. The blend starts with one decay step already applied, so its first tick already moves. A real lift releases at the coasted position, with no step back. A held lift that does not coast (too slow, or more than `COAST_AFTER_MS` after the last sample) stays where the output already is until it rejoins or expires. It used to drop to the last sample in one tick (a back-step of up to the whole prediction, 30 px at 1000 px/s), and after the first review it faded back instead, which still moved the output against the stroke just before the release and turned the browser's fling velocity negative. Lifts are held only for a contact that never shared the screen with another finger (checked after every frame, so a finger that rejoins next to a second one counts too): a pinch or a two-finger gesture releases at once, so its fingers do not glide apart after they leave (a 300 px/s pinch used to end 22 % wider than the fingers were). A lift is also not held while another finger is down, even one that landed in the same frame, and a new finger landing in another slot releases a held lift in its own frame first, so the browser never sees a gliding contact next to a new touch. `WALLVIEW_TOUCH_HOLD_MAX_PX_PER_S` (default 0, off) also releases at once above that speed.
- **Prediction.** Velocity is a least-squares fit over the last three samples (each gap at most `WALLVIEW_TOUCH_MAX_GAP_MS`, default 150). Prediction is capped at the smaller of `WALLVIEW_TOUCH_PREDICTION_MS` (default 30) and the last sample interval, and fades out over four output ticks (four timer periods, or four display frames in vblank mode) once no sample came for 1.5 intervals or `WALLVIEW_TOUCH_PREDICT_EASE_MS` (default 100), whichever is longer, so a stopping finger does not snap back. Prediction lives for `MAX_GAP_MS` or `PREDICT_EASE_MS` plus the fade, whichever is longer, and the fade starts at the latest four ticks before that, so it has ended when prediction stops: before that clamp a prediction still at full length was dropped in one tick and the output stepped back.
- **No prediction for a resting finger.** Prediction starts only after the contact has travelled `WALLVIEW_TOUCH_PREDICT_TRAVEL_PX` (default 13) from where it went down, which halves tap and long-press jitter (2.8 to 1.4 px in the replay) and keeps a tap that slides a little inside the browser's tap radius (see the nineteenth review).
- **Idle cost.** With no finger the daemon has no timer and sleeps in `poll`. A finger that rests (no new coordinates for longer than the prediction lifetime (see Prediction) plus `LATENCY_MS`, 166 ms at the defaults; no hold, no blend) also stops the timer until the next position change (40 % fewer ticks on the capture, identical output); frames that change only `ABS_MT_TOUCH_MAJOR` do not wake it (before, each one armed the timer for one tick: 493 wakeups in 10 s of a resting finger with a width change every 20 ms). The first position change after such a rest is sent at once instead of one timer period later (unless it is a jump of more than `REJOIN_PX` within `GAP_BLEND_MS`, which is blended as above). The code is locked in memory (`mlockall`) so the first touch after a long idle never waits for page faults, and a failure to get `SCHED_FIFO` is logged.
- **Start and stop.** The daemon grabs the touchscreen only while no finger is down (it waits up to 30 s), because a grab during a touch leaves the browser with a contact that never lifts. After the grab it empties the stale queue and reads the kernel's slot state right after, so a touch that lands during start-up is followed from the first frame. `ABS_X`/`ABS_Y` follow the oldest contact, not the lowest slot, and `BTN_TOUCH` changes at most once per frame (one contact ending and another starting in one frame used to send 0 and 1). `S35wallview-touch` runs the daemon as a background child that its loop kills on `TERM`; `stop` then waits a second and repeats `killall` until no daemon is left, so a daemon still starting while `stop` runs cannot stay behind holding the grab. On `SIGTERM` or a lost input device it releases every contact it reports before destroying the virtual device; the signals are only unblocked inside `ppoll`, so a stop request cannot be missed while the daemon sleeps.
- **Display-locked output (issue #25, off by default).** `WALLVIEW_TOUCH_VBLANK=1` resamples once per display refresh instead of on its own 120 Hz timer, the way Android's input consumer is driven by the display: while a finger is down it queues a DRM vblank event on `WALLVIEW_TOUCH_DRM` (default `/dev/dri/card0`) and resamples `WALLVIEW_TOUCH_VBLANK_OFFSET_MS` (default 10) after each vblank, so the new position is in the browser's queue several milliseconds before it starts the next frame instead of racing its page-flip event. The frame length is measured from the vblank timestamps (it locks between about 40 and 120 Hz, seeded at 60 Hz); the lift-blend decay is scaled to it. A timer at three frames keeps output going if vblank events stop, and with the display off it falls back to the 120 Hz timer.
  - DRM master safety: opening a primary node with no master makes the opener master. The node is therefore opened only when another process already has it open (a `/proc/*/fd` scan comparing device numbers, retried with a backoff from 5 s, doubling up to 60 s; `WALLVIEW_TOUCH_DEBUG=1` logs how long it took), only if `DRM_IOCTL_VERSION` names `WALLVIEW_TOUCH_DRM_DRIVER` (default `rockchip`), and if `DRM_IOCTL_DROP_MASTER` succeeds anyway the node is closed again (logged once). A failing `DRM_IOCTL_WAIT_VBLANK` (display off) falls back to the timer and is retried once a second, not every tick, and a read error on the node switches to the timer at once instead of after the three-frame fallback. A non-master, unauthenticated fd may queue vblank events (`DRM_IOCTL_WAIT_VBLANK` has no `DRM_AUTH` flag).
  - While the fd stays open, a browser exit no longer is the last close of the node, so fbcon is not restored between browser restarts; the next browser still becomes master. Test a browser restart with the mode on before making it the default.
  - Reviewed (Fable 5.2, 2026-11-28): GO as opt-in and for a locked trial; the phase offset, the master gate, the driver check, the double-resample guard and the error handling come from that review. Way back: remove the line from `/data/wallview/touch.env` and restart `S35wallview-touch`.
  - The latency and prediction settings stay as they are; Android's 5 ms and 8 ms do not suit this controller (see "Comparison with Android touch resampling").
- **Debugging.** `WALLVIEW_TOUCH_DEBUG=1` logs every lift that could be held (age, speed, last three `ABS_MT_TOUCH_MAJOR` values, hold or release; taps with a single sample, multi-touch contacts and lifts with `LIFT_MS=0` are released without a log line), every rejoin or reject (distance from the prediction, limit) and every expiry to `/tmp/wallview-touch.log`. That log is what the lift hold and the rejoin radius should be set from.
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

Ninth review (round 5 of the loop, on 11323c4). No stuck, phantom or lost contact and no protocol violation (about 16 million fuzzed output frames); nothing in patch 0006, the init script or the build. Applied:

- The gap blend started only above `HOLD_RECENT_MS` (100 ms), but at the controller's cadence a bridged false lift or a missed report is often a 60–98 ms gap; all 13 remaining steps of 50 px or more in the replay followed such gaps. It now has its own lower bound (`GAP_BLEND_MIN_MS` 60) and applies only to moves above `REJOIN_PX`, so a small first move after a rest of 100–280 ms is no longer delayed (it reached its position only after 38 ms).
- Every blend, rejoin or gap, showed the already shown position for one more tick before it started to move; the first decay step is now applied at once.
- Resync could apply a frame that the kernel queued between the drain and the slot ioctls to the wrong slot (its leading events carry no `ABS_MT_SLOT`); a harness showed one finger's X moved 180 px onto another. Resync now reads the input once more after the ioctls and repeats while events arrive.
- Replay with patch 0006: max step 98 to 65 px, jerks 511 to 490, stalls 403 to 402, path error p50/p95 7.6/37.4 to 7.6/36.8 px. Without the patch: jerks 444 to 435, path error 7.2/32.0 to 7.0/31.8 px.
- Tests: jump after a 70 ms and a 200 ms gap, small first move after a 200 ms rest.

Considered and not done: with patch 0006 a bridged false lift is invisible to the daemon, so the prediction fade (after `PREDICT_EASE_MS`, 100 ms) walks the output back while the finger is still moving and the next sample pulls it forward (11 such fade-backs in the replay, worst 27 px). Waiting longer before the fade (Fable's variant: until `GAP_BLEND_MS`) removes them, but every finger that really stops then shows the prediction ahead of it for 280 ms, and a slider lifted in that time releases at the overshoot. `WALLVIEW_TOUCH_PREDICT_EASE_MS=250` gives that variant for the panel A/B (it took effect only from the eleventh review on; before, the fade was clamped to end at `MAX_GAP_MS` and any value above about 117 ms behaved like 117).

Tenth review (round 6 of the loop, on 2090d3e). Neither reviewer found a stuck, phantom or lost contact, a protocol violation, or anything in patch 0006, the init script or the build. A new convergence fuzzer ticks only while the timer is armed, as on the panel, and checks that after every silence the output equals the kernel state; it ran 16 million output frames with no failure. Applied:

- When a sample came less than the latency after one that ended a gap over `MAX_GAP_MS`, the output jumped to the newer sample and stepped back 8–12 px on the next tick. Most likely with a second finger moving, or with a larger `LATENCY_MS`. The output now holds the first sample after the gap until interpolation reaches it.
- The gap blend's move threshold was |dx| + |dy|, while `REJOIN_PX` is a straight-line distance everywhere else; diagonal moves of 22–30 px were blended instead of sent at once. It is now straight-line. Replay with patch 0006: jerks 490 to 516, stalls 402 to 407, path error p50/p95 7.6/36.8 to 7.5/36.6 px (fewer moves are blended). Without the patch: jerks 435 to 462, stalls 379 to 382, path error p50 7.0 to 6.9 px.
- Test: second sample soon after a long gap. It fails on 2090d3e.

Feel trade-off for the panel A/B, not changed: at the controller's slow cadence a normal report 60–75 ms after the previous one also passes the gap blend (47 of the 56 blended 60–100 ms gaps in the capture had no unpressed read). On a steady fast swipe that adds about 6 ms of mean lag (1000 px/s with 65 ms reports: 26.0 ms instead of 19.3 ms), while it halves the largest steps (2000 px/s: 28 instead of 48 px). A gate on deviation from the predicted path removes the lag but gives back most of the smoothing, so the data cannot tell the two cases apart. `WALLVIEW_TOUCH_GAP_BLEND_MIN_MS=100` against the default 60 goes on the A/B list with `PREDICT_EASE_MS` and `LATENCY_MS`.

Eleventh review (round 7 of the loop, on f37cd64). Again no stuck, phantom or lost contact and no protocol violation at the default options. Applied:

- With non-default options (`RATE=60`, `BLEND` 0.8–0.9 or `MAX_GAP_MS=100`) the output could come to rest 1 px off the finger: in the tick where the blend offset fell below 0.5 and was zeroed, the emitted position still held the offset, and the same tick stopped the timer. A tick that applied an offset now keeps the timer for one more tick. The convergence fuzzer now also runs with those options (`RATE` 30 and 60, `REJOIN_SLACK=0.9` with `MAX_GAP_MS=100`): about 50 million output frames, no failure.
- `PREDICT_EASE_MS` above about 117 ms had no effect, because the fade was clamped to end at `MAX_GAP_MS`. Prediction now lives for `MAX_GAP_MS` or `PREDICT_EASE_MS` plus the fade, whichever is longer, and the timer stays armed for that time. The default (100) is unchanged; replay with patch 0006 and `PREDICT_EASE_MS=250`: 5118 ticks while down instead of 4845, jerks 512 instead of 516, stalls 413 instead of 407, path error p95 36.8 instead of 36.6 px.
- Patch 0006 refreshed a slot's keep deadline on its second unpressed read, so an unpressed read, a second one 100 ms later and then silence released the finger 210 ms after the first instead of 110 ms. The deadline is now set only by the first unpressed read, so 110 ms bounds the extra delay of every real lift. The replay models the same; its numbers on the capture are unchanged (the capture has no such sequence).
- `resync()` could read the slots in the middle of a kernel frame (a new tracking id with the slot's old position), since the ioctls are not synchronised with the event queue. It now takes the ids and positions twice and repeats until both agree with each other and with no input pending.
- `WALLVIEW_TOUCH_RATE` is now documented (see above).
- Test: prediction still alive 165 ms after the last sample with `PREDICT_EASE_MS=250`. It fails on f37cd64.

Twelfth review (round 8 of the loop, on df08388). No stuck, phantom or lost contact, no protocol violation and nothing in patch 0006, its replay model, the init script or the build. Both reviewers ran the convergence fuzzer with non-default options (`RATE` 30/60/1000, `BLEND` 0.9, `MAX_GAP_MS` 100, `PREDICT_EASE_MS` 250, `LATENCY_MS` 0–60, a vblank-mode variant and 60 random option sets): no failure. Applied:

- A real lift during a gap or rejoin blend was released where the blend had got to, up to about 50 px short of the finger: `release()` skips the last sample after prediction to avoid a back-step, and a blend counted as prediction. That happens when lifts are not held: two fingers down, or `LIFT_MS=0` (in the replay without patch 0006, worst 50 px). A lift with a blend offset that trails the motion now ends on the last sample, which is a step forward. Replay at the defaults unchanged. (This rule looked only at the sign of the offset and stepped back in other cases; the thirteenth review replaced it.)
- The second snapshot in `resync()` came microseconds after the first, so both could fall inside one kernel frame (new tracking id, old position) and agree. It now waits 300 µs between them; resync runs only at start-up and after `SYN_DROPPED`.
- `WALLVIEW_TOUCH_BLEND` above 0.95 fell back to 0.7 without a log line; it now logs like every other ignored value.
- "Idle cost" said a resting finger stops the timer after `MAX_GAP_MS`; it is the prediction lifetime plus the latency (166 ms at the defaults).
- Tests (each fails on the commit before its fix): the test clock now ticks only while the timer is armed, as on the panel; a blend settles exactly on the finger at 60 Hz with `BLEND` 0.9 and `MAX_GAP_MS` 100 (273 cases, 208 off on f37cd64); two fingers lifted 9–25 ms into a gap blend release on the last sample (fails on df08388); `resync()` against a fake evdev whose position changes between the snapshots, and which releases a contact the kernel no longer has (fails on f37cd64).

Considered and not done: the timer stays armed for the prediction lifetime after every sample even for contacts that cannot predict (taps, long presses, a finger resting after less than `PREDICT_TRAVEL_PX`). Stopping it for those cuts replay ticks while down from 4845 to 3984 with patch 0006 (4611 to 3659 without), but slow fingers then take the sent-at-once path more often (without the patch path error p50 6.9 to 7.2 px, jerks 462 to 465, stalls 382 to 326). A tick costs microseconds, so it stays as it is; it is on the panel A/B list if idle power matters.

Thirteenth review (round 9 of the loop, on 7085aaa). Both reviewers found the same regression in the twelfth review's release rule; nothing else in the daemon, patch 0006, the replay model or the build. Applied:

- The rule "a blend offset that trails the motion ends on the last sample" stepped back in two cases. A few ticks into a blend the offset is small but prediction has already carried the output past the last sample, so the release jumped back to it (two fingers, `LIFT_MS=0`, or one finger whose hold was refused: in 200,000 random strokes 14,000 back-steps, worst 97 px; in the replay with patch 0006 and `LIFT_MS=0` 13 releases, worst 67 px). And after a rejoin that landed behind a coast overshoot the velocity is 0 (sample gap over `MAX_GAP_MS`), which counted as trailing, so the release went back to the finger (−18 px; −26 px when the lift came in the frame right after the rejoin, before any tick, which was also the case before the twelfth review). `release()` now decides by where the output is: after a blend it ends on the last sample only if the output is not ahead of it along the last move (the vector from the previous sample, or the velocity if those are equal); otherwise as before. Random strokes: 0 back-steps with one finger, 15 (worst 17 px) with two fingers, the same as on df08388; the forward step onto the finger for an early lift is kept.
- The replay now counts release frames that step back more than 3 px; the step and jerk counts saw only timer ticks, so the regression did not show in its summary. On the capture: 0 in all four combinations of patch model and `LIFT_MS` 0/80 (7085aaa: 13, worst 67 px, with patch 0006 and `LIFT_MS=0`).
- `WALLVIEW_TOUCH_RATE` outside 2–1000 is now logged when ignored.
- `S35wallview-touch` removes its PID file when the loop ends because resampling was turned off, so a later `stop` cannot kill an unrelated process that reused the PID and `start` is not skipped.
- Tests: two fingers lifted 9–93 ms into a gap blend never step back and end on the finger when the output trails it (fails on 7085aaa); a lift 0–25 ms after a rejoin behind the coast does not step back (fails on 7085aaa and df08388).

Fourteenth review (round 10 of the loop, on bc9eb32). Nothing in the daemon's contact handling, patch 0006 or the build; three release cases and two small script issues. Applied:

- After a rejoin that landed on or behind the last sample before the false lift (a finger that slowed during the lift), the release direction came from those two samples and pointed against the stroke or was zero, so a lift right after the rejoin went back from the coasted position to the sample (up to 46 px; 384 back-steps in 200,000 random strokes, worst 53 px). Each contact now keeps its stroke direction: the last move that changed the position, or the lift velocity at a rejoin.
- A sample followed by its lift before the next tick was emitted even when prediction had already carried the output past it (the 15 two-finger back-steps of the thirteenth review, worst 17 px). That case now uses the same "not ahead" test. Random strokes now: 0 release frames stepping back more than 3 px with one or two fingers; what remains are 1–5 px clamp effects at the screen edge. Ticks of a held lift can still move back while a rejoin blend pulls the output to a rejoin that landed behind the coast; that is the blend, not the release.
- The replay's release back-step count was meant to also count a step against the finger's last movement (it did not work until the fifteenth review). Capture: 0 in all four combinations.
- `REPLAY_KEEP_UNPRESSED=0` turned the patch model on; only a positive number does now.
- `S35wallview-touch start` does nothing when resampling is off, so the loop's own PID file removal cannot race the parent writing it.
- Test: a lift 0–25 ms after a rejoin 10 px behind, on, or 10 px past the last sample does not step back (fails on bc9eb32).

Fifteenth review (round 11 of the loop, on ee5f45e). No stuck, phantom or lost contact and no protocol violation; no release frame stepped back against the finger. The findings were releases that end short of the finger or on the wrong side of it. Applied:

- The velocity fit over three samples straddled a reversal: for a finger that turned back, it pointed against the last move, so prediction, the hold and the coast carried the output the wrong way (synthetic: a finger at 98 turning back to 133 was released at 88 with a fling in the old direction). When the fit points against the last move the velocity is now 0. Not present in the capture (replay unchanged).
- In the tick where a blend offset reached zero, the contact was still marked as predicted although the output trailed the sample, so a lift right after it froze up to about v × `LATENCY_MS` short of the finger (13–17 px in a two-finger case, at half the tick phases). That tick now keeps the interpolation's own state.
- A move of up to 4 px against the stroke (controller jitter while the finger settles) replaced the stroke direction, so a blended release counted as ahead and stayed short of the finger. Such a move no longer replaces it.
- A held lift whose coast started before the output had reached the last sample (a jump right before the lift) was released where the coast ended, short of the sample (29 px in the scenario, worst 45 px in 300,000 random strokes). Every predicted, coasted or blended release now goes to the last sample when the output is not ahead of it; for a coast "ahead" is measured along the coast velocity.
- The fourteenth review's change that kept the prediction when a sample was followed by its lift within one tick is kept: reverting it gave releases at the overshoot of a finger that stopped. A finger that stops shows in the sample (no move along the stroke), and the prediction fade covers the slower cases.
- The replay's against-the-finger term never evaluated (it read the raw path at the release time, after the raw lift); it now uses the last raw move. The replay also prints the distance of each release from the last raw position (the coasted flicks dominate the tail). Capture: 0 release back-steps in all four combinations, identical to ee5f45e.
- Random strokes compared with ee5f45e (Fable's and Opus's fuzzers): releases more than 3 px short of the last sample 3125 → 252 (with false lifts and reversals), 3765 → 46 (reversals), 142 → 0 (no reversals); release frames against the finger's last move 6 → 39 and 10 → 15, all at most 8 px; no change in the rejoin harness. (Correction from the sixteenth review: the two-finger harnesses had been built against the working copy instead of the file under test, so this line first said they were unchanged. Measured properly, releases more than 5 px short of the finger in Opus's random two-finger strokes went from 1868 to 144, while releases more than 3 px past a finger that stopped right before the lift went from 3258 to 6001, see below.)
- Tests (each fails on ee5f45e): two fingers lifted right after a gap blend settled, at every tick phase; a finger that turns back and lifts; a held lift during a gap blend.

Known and left: a two-finger release during a gap blend where the finger reversed by more than 4 px just before the lift still ends where the blend has got to (up to 27 px short in the synthetic case); telling a real reversal from noise there needs more samples than the controller gives.

Sixteenth review (round 12 of the loop, on db60f63). Fable found nothing; Opus found two cases brought in by the fifteenth review. Applied:

- The velocity reversal guard also returned speed 0 when a finger settling at the end of a drag reported a last move of 1–2 px backwards, so a false lift at that moment was not bridged and the drag split into two touches. `velocity()` now zeroes only the direction and still returns the fitted speed, so the hold decides on the real speed; the coast takes its speed from the zeroed direction and does not run the wrong way.
- The jitter guard could lock the stroke direction for a whole slow drag (up to about 160 px/s) when the first move after touch-down was a small roll back, so a later release snapped 1–2 px back. A move that agrees with the previous move now also replaces the direction.
- A finger that stopped (or slowed to a few px) right before a lift within one tick of its last sample was released at the prediction overshoot, up to 49 px past it with a second finger down or `LIFT_MS=0` (6001 of 13,125 such lifts on db60f63, 3258 on ee5f45e). When the fresh sample's progress along the stroke is smaller than the overshoot, the release now goes to the sample: 49 lifts more than 3 px off, worst 4 px. The price is a step back of up to about 20 px onto a decelerating finger in the same rare case (a sample and its lift less than one tick apart); a stopped finger ends where it stopped, which matters more for a slider.
- Tests (each fails on db60f63): a drag that settles with 2 px of jitter and then has a false lift stays one touch; a slow drag after a 3 px roll back at touch-down does not step back at the release; a finger that stops and lifts within a tick with a second finger resting is released on the sample.

For the panel A/B: `BLEND` decays once per output tick, so with `RATE=60` a blend takes twice as long in time as at 120 Hz (vblank mode rescales it to the display frame). Change `BLEND` together with `RATE` when comparing rates.

Seventeenth review (round 13 of the loop, on 74e8c2c). Applied:

- **The position sent in the lift frame never reached the browser.** `release()` put the final `ABS_MT_POSITION_X/Y` and `ABS_MT_TRACKING_ID -1` of a slot into one `SYN_REPORT`. libinput (`evdev-fallback.c`, `fallback_flush_mt_events`) turns such a slot into a plain `touch_up`, which carries no coordinates, and drops the motion; Cog's DRM platform takes x/y only from down and motion events. So the browser's last touch point was the previous tick's output: the prediction overshoot, the blend or the coast position. None of the release-position corrections since the twelfth review had any effect in the browser. Replay of the capture with a libinput model, lifts whose final position was lost: 12 of 64 (worst 18 px) with patch 0006, 21 of 81 (worst 50 px) without it and with `LIFT_MS=0`. `release()` now sends the slot's pending position in its own frame before the lift; it costs one extra `write()` for a lift that moves. Replay: release distance from the last raw position p95 4 px instead of 26 px without the patch and with `LIFT_MS=0`; the other combinations are dominated by coasted flicks, as intended.
- The offline test and the replay now model libinput: a position in the same frame as the slot's lift is ignored. Every release-position test written since the twelfth review fails on 74e8c2c under this model.
- A false lift right after the finger turned was not bridged: with the reversal guard the held lift had no direction, so the rejoin limit collapsed to `REJOIN_PX` around the last sample and the finger, still moving the new way, landed outside it. When the fitted direction is zeroed but the last move is at least `HOLD_TRAVEL_PX`, the hold now uses the velocity of that last move. In Fable's grid of turned strokes 78 of 189 are split instead of 180 (51 for straight strokes with the same gaps). Test: a false lift right after the finger turned back stays one touch.

With the final position now visible, the documented release choices are what the browser gets: a lift that is not ahead of the last sample ends on it; a coasted or predicted output that is ahead stays; a finger that stopped within a tick of its lift ends on the sample, a step back of up to about 20 px in that rare case, which also lowers the browser's fling velocity for a finger that was stopping anyway.

Eighteenth review (round 14 of the loop, on 1917abf). Applied:

- **Cog dispatches one touch event per libinput frame.** libinput emits the touch events of one evdev frame in slot order and then one `TOUCH_FRAME`; Cog's DRM platform (`input_handle_touch_event`, the same in 0.16 to 0.18 and master) sends WPE one event per `TOUCH_FRAME` whose type and id come from the last touch event of the frame, and WebKit (`WebEventFactory` `stateForTouchPoint`, 2.50) marks every other point `Stationary`. WebCore registers a touch target only on `Pressed` and removes it only on `Released`, so a down or up that shares a frame with a higher slot is lost for good: two fingers landing in one controller scan gave the page a one-finger gesture (pinch failed), two fingers lifting in one scan left the lower one in `event.touches`. In Fable's libinput + Cog model 1350 of 2000 random two-finger gestures lost a `touchstart` or `touchend`. The raw kernel device has the same problem; the daemon is the place that can remove it. Every frame now carries the events of one slot only (`queue()` flushes when the slot changes), lifts of a frame were sent after its new contacts (reversed in the nineteenth review), and `BTN_TOUCH` follows the contacts already sent, so it does not toggle when one finger lifts and another lands in the same scan. Model: 0 of 2000 lost; frame checker over 44 million frames: no frame mixing a contact change with another slot, no `BTN_TOUCH` mismatch. Cost: one extra `write()` per slot that changes in a tick; with two fingers moving, each also gets its own `touchmove` now (before, only the higher slot was `Moved`).
- The turned-stroke hold velocity of the seventeenth review also drove the coast, so a real lift right after a turn glided 20–35 px further back and the browser got a backward fling. That velocity now serves only the rejoin; a turned lift does not coast and ends on the last sample. Test: a real lift right after the finger turned back ends within 3 px of it (32 px on 1917abf).
- The offline test now also fails on any frame that changes a contact together with another slot (5 such frames in its pinch case on 1917abf).
- Checked from the WebKit 2.50 source: the final position frame and the lift arriving microseconds apart do not distort the fling velocity, since `ScrollAnimationKinetic::computeVelocity` divides the summed deltas of the last 150 ms by the span between the first and last event. libinput ignores `BTN_TOUCH` and `ABS_MT_TOUCH_MAJOR` on multitouch screens.

Nineteenth review (round 15 of the loop, on f8f3c7b). Opus 5.5 found nothing; Fable 5.2 found that the eighteenth review reasoned from Cog alone, while the kiosk runs `wallview-browser` (WPEPlatform DRM, `S99wallview-kiosk`) and uses Cog only when `/data/wallview/browser` says `cog`. WPEPlatform (`WPEDRMSeat.cpp`, WebKit 2.50.1) sends every libinput touch event on its own, so the per-frame typing problem of Cog does not exist there, but its `GestureDetector` follows one touch sequence: a down while another sequence is open is ignored, an up clicks at the remembered down position, and a move further than `WPE_SETTING_DRAG_THRESHOLD` (8 logical px, 12 panel px at `WPE_DRM_SCALE=1.5`) from the down turns the touch into a drag for good, with no click. Applied:

- **A lift is sent before a landing in the same scan.** With the lifts deferred after the new contacts, a tap followed by a second tap whose finger landed in the scan in which the first lifted reached the browser as two overlapping touches: in Fable's libinput + WPEPlatform model the second target was never clicked in 2000 of 2000 pairs (971 clicked the first target twice, 1029 scrolled by the distance between the two). Cog's fallback point is the lowest active seat slot, so it also lost the second click. `handle_frame()` now handles the lifts of a frame first, and a lift is not held when another finger lands in the same scan (a landing in another slot released held lifts anyway). `BTN_TOUCH` now goes 0 and then 1 in two frames in that case, which libinput ignores. Models: both taps clicked in 2000 of 2000 pairs; Cog model 0 of 2000 gestures lost; frame checker over 111 million frames with four option sets: no frame mixing a contact change with another slot. Test: the lift of one contact comes before the landing of another in the same scan (it came after on f8f3c7b).
- **A sloppy tap stays a tap.** Prediction (up to 30 ms of velocity) and the coast of a held lift pushed a tap that slid 7–12 px past the 12 px tap radius, so the page got a small scroll instead of a click: in Fable's model 1251 of 4224 taps sliding 2–12 px lost their click (0 of 1920 up to 6 px, 328 of 384 at 12 px; 973 still with `COAST_MS=0`). `PREDICT_TRAVEL_PX`, `START_TRAVEL_PX` and `HOLD_TRAVEL_PX` now default to 13, so no prediction, hold or coast starts while the finger is inside the radius. Model: 0 of 4224 lost. Replay: unchanged downs (77 and 64), path error p95 31.8 to 32.1 px (36.6 to 36.8 with patch 0006), stalls 382 to 405 (407 to 430), jerks 462 to 465; fuzzers in `/tmp` equal or slightly better. The 36 taps of the 2026-11-27 capture move at most 2 px, so the panel trial should include deliberately sloppy taps. Test: taps sliding 2–12 px in 2–4 samples never leave 12 px around the down point (14 px on f8f3c7b). Cog's tap limit is looser (40 px or 200 ms) and was not modelled.
- **Patch 0006 also releases a kept contact before a new finger lands.** A real lift is kept until the read with point count 0 (median 46 ms later), so a second finger landing in that window under another id produced the same overlap in the kernel, where the daemon cannot reorder it. When a read reports a pressed finger whose slot is not active, the patch now first releases, in a frame of their own, the kept contacts that this read does not report pressed. Not verified on hardware: the controller's id allocation for a second finger needs a two-finger capture. If the controller reuses the first finger's id for the new finger, the patch still continues the old contact at the new position (one touch that jumps, so no click); see the distance gate under "Considered and not done" in the fifth review.

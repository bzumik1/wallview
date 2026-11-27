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

The controller firmware, its update protocol and what it would take to raise the scan rate are in [touch-controller.md](touch-controller.md) (parked, with the steps to resume). On the panel side `wallview-touch` (`buildroot/board/wallview/touch/`) grabs the touchscreen and feeds the browser a virtual touchscreen at a steady 120 Hz: positions interpolated from the last samples `WALLVIEW_TOUCH_LATENCY_MS` (default 8) behind real time and predicted at most `WALLVIEW_TOUCH_PREDICTION_MS` (default 12) ahead of the last sample; touch down and up pass through at once.

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

## Why touch scrolling misses frames (WebKit 2.50.5, WPEPlatform DRM, 2026-11-27)

Method: `perf record -e cpu-clock --switch-events -k CLOCK_MONOTONIC -t <tids>` on the web process main, scrolling, compositor, `EventDispatcher`, IPC and Skia threads and on the UI process main and vblank threads while `touchplay` swipes, together with the `flipmon` vblank times (`tools/perf/threadtrace.sh`, run on the panel as `/tmp/cap.sh > capture.txt`; it picks the web process whose compositor thread has used the most CPU). The context switches give a per-thread timeline (when each thread runs and for how long, symbolised leaf samples say what it does) that is laid over every vblank without a new frame: `tools/perf/threadtrace.py <unstripped libWPEWebKit> capture.txt misses`, or `window <from> <to>` for any interval. The kernel has no ftrace/uprobes, so this is the finest view available without rebuilding. Two measurement caveats:

- `flipmon` sees the new `fb_id` as soon as the atomic commit ioctl returns, not when the frame is latched (WPEPlatform commits non-blocking with `IN_FENCE_FD`). A frame that is committed too late is therefore still counted at its vblank, and the miss shows up one vblank later, when the compositor is still waiting for the page-flip event. The number of frames is right, the attribution is one frame late.
- On Home Assistant views the `missed` value of `touchtest.py` also counts a dashboard update that happens after the scroll has stopped as one long gap. For A/B runs count instead the vblanks without a new frame from 100 ms after touch down to 150 ms after release (18 vblanks per swipe, the content has to move in every one of them); configurations are compared interleaved in the same browser session where possible, because the level drifts between sessions (9–17 % on the same build).

One frame on the climate view needs about 1 ms from vblank to the scrolling thread, 6–7 ms of compositor CPU, 4–5 ms for the GPU and the DRM commit, i.e. 12–13 ms of the 16.7 ms. Any wait of a few milliseconds in that chain loses the frame.

Four independent causes, in the order of a swipe:

1. **The first scroll frame waits for the main thread.** `WebPageProxy::updateDisplayLinkFrequency()` asks for full-speed display updates as soon as wheel events arrive, but `DisplayLink::incrementFullSpeedRequestClientCount()` only counts the request (it does not start a stopped link) and `DisplayLink::notifyObserversDisplayDidRefresh()` skips every client without an observer. Observers are added by the web process main thread when it schedules a rendering update, so `ThreadedScrollingTree::displayDidRefresh()` is not called before that. Timeline on a page with a 400 ms busy loop: wheel events reach the scrolling thread 17 ms after touch down and every 8 ms from then on, the compositor draws nothing until the main thread is free (first frame after 250–420 ms). Fix: patch `0007`.
2. **The scrolling thread does not commit while the main thread is busy.** `ThreadedScrollingTree::displayDidRefreshOnScrollingThread()` applies layer positions only when `m_state != Idle`; in `Idle` it arms the 1 ms desynchronisation timer only if `m_renderingUpdateWasScheduled`, which the main thread sets in `Page::didScheduleRenderingUpdate()`. A kinetic scroll moves in `serviceScrollAnimations()` at the display refresh and the main thread clears the flag in `willStartRenderingUpdate()`, so a main-thread task that starts between two rendering updates stops all scroll frames until it ends: the scrolling thread still runs at every vblank and handles wheel events, the compositor stays idle. Wheel events, their replies (`EventDispatcher::internalWheelEvent()` answers from the scrolling thread) and the gesture recognition in the UI process (`ViewPlatform::handleGesture()` runs beside `handleTouchEvent()`, it does not wait for the page) do not depend on the main thread. Fix: patch `0004` (apply layer positions on every display refresh). Checked on the panel with a copy of the library in `/tmp` in which the one conditional branch is replaced (`LD_LIBRARY_PATH`): page with a 40 ms busy loop every 150 ms 13–19 missed frames per 8 swipes → 0, with a 400 ms loop → 0, static page unchanged at 60.
3. **The compositor waits for tile painting.** `CoordinatedPlatformLayer::flushCompositingState()` takes the pending backing store update and `CoordinatedBackingStoreTile::processPendingUpdates()` calls `waitUntilPaintingComplete()` and `serverWait()` (EGL fence) for every tile buffer. On the Home Assistant views the compositor thread wakes at the vblank, sleeps 4–55 ms while both `SkiaGPUWorker` threads run and then needs up to 25 ms for the texture updates. The largest burst comes right after touch down (the page repaints the pressed card), smaller ones whenever the main thread commits new tiles; during a drag the result is a frame only every other vblank. This is the largest cause on the Home Assistant views. Fix: patch `0006` (a layer keeps its old tiles for up to two frames while its new ones are being painted).
4. **The frame threads are not real-time.** WebKit runs the scrolling thread, `EventDispatcher` and the UI process IPC queues with `SCHED_RR` 5 (`Thread::QOS::UserInteractive`, `RealTimeThreads`), but `ThreadedCompositor` (`CompositingRunLoop.cpp`), the Skia painting workers, `WPEScreenSyncObserverDRM` (the vblank thread that drives the `DisplayLink`) and the UI process main thread (input, IPC, DRM commit) are normal threads. With HZ=250, no `HRTICK` and the web process main thread at 100 % they are woken up to 7 ms late. `chrt -r -p 5` on these threads, interleaved with resetting them in the same session, climate view: 17.4 / 15.6 / 12.8 % → 14.9 / 12.5 / 10.8 % missed vblanks for compositor + vblank thread, 11.8 / 15.3 / 15.6 → 7.3 / 10.4 / 13.9 with the Skia workers added, 15.6 / 18.4 → 7.3 / 9.4 with the UI main thread as well. Fix: patch `0005` and `sched_setscheduler(SCHED_RR | SCHED_RESET_ON_FORK)` in `wallview-browser`.

Patches (all apply to the 2.50.5 tree with `patch -p1 --dry-run`, the changed files compile with the CI flags):

- `0004`: scrolling thread commits layer positions on every display refresh. Effect checked on the panel (binary-patched copy, see 2).
- `0005`: compositor, Skia painting workers and DRM vblank thread as real-time threads; `wallview-browser.c`: UI main thread. Effect checked with `chrt` (see 4).
- `0006`: compositor does not wait for unfinished tiles. Not yet run on the panel.
- `0007`: display link serves full-speed clients without observers; needs `0004` to have an effect. Not yet run on the panel.

Missed vblanks in the drag window (% of 288 per value, each value 2 runs of 8 swipes after a browser restart; "real-time" = `chrt -r -p 5` on compositor, Skia workers, vblank thread and UI main thread, i.e. what `0005` and the launcher do):

| View | 2.50.5 + `0001`–`0003` | + real-time | + `0004` | + `0004` + real-time |
|---|---|---|---|---|
| climate | 5.9 / 16.0 | 8.0 / 9.0 | 15.3 / 14.6 | 8.7 / 5.2 |
| settings | 12.8 | 8.7 | 30.6 | 6.6 |
| lights | 5.9 | 3.8 | 3.1 | 1.0 |

`0004` without the real-time threads is not better and can be clearly worse on the Home Assistant views: every vblank now starts a composition, and each of them can run into cause 3 with a compositor that is also woken late. `0004` and `0005` belong together; the rest of the misses is cause 3 (`0006`).

All seven patches are in the build. Two runtime switches allow A/B measurements with one image (environment of `wallview-browser`, inherited by the web process): `WALLVIEW_WEBKIT_NO_SCROLL_COMMIT=1` restores the old behaviour of `0004` (and so disables `0007`), `WALLVIEW_WEBKIT_TILE_DEFER=<frames>` sets the deferral limit of `0006` (default 2, `0` waits for every tile as before). To build a library locally the Lima VM has a configured tree (`~/build/wk2`: `orig` pristine, `src` with the patches, `build` configured like CI); one WebCore unified source needs about 2.5 GB of compiler memory, so the 10 GB VM can run 3 jobs at most. A library can be tried without installing it: copy `libWPEWebKit-2.0.so.1` and the `libWPEPlatform*` libraries to `/tmp/<dir>` and start the kiosk loop with `LD_LIBRARY_PATH=/tmp/<dir>`.

Measured with the CI image of PR #9 (all seven patches, 2026-11-27), switches as above, vblank thread set to `SCHED_RR` 5 with `chrt` right before each run (the image's `0005` did not promote it, fixed since). Missed vblanks in the drag window, two interleaved rounds of 8 swipes per value, total in brackets:

| Configuration | climate | settings | lights |
|---|---|---|---|
| all patches | 3.8 / 13.7 (8.7) | 36.5 / 46.8 (41.7) | 0.7 / 3.3 (2.0) |
| `WALLVIEW_WEBKIT_TILE_DEFER=0` (no `0006`) | 12.2 / 15.5 (13.8) | 36.8 / 39.7 (38.3) | 1.3 / 3.3 (2.3) |
| `WALLVIEW_WEBKIT_NO_SCROLL_COMMIT=1` (no `0004`, `0007`) | 5.9 / 7.6 (6.8) | 49.4 / 40.0 (44.7) | 4.6 / 5.8 (5.2) |
| both switches (real-time threads only) | 14.8 / 28.4 (21.6) | 34.2 / 47.7 (41.0) | 5.9 / 6.5 (6.2) |

The patches together cut climate from 21.6 to 8.7 % and lights from 6.2 to 2.0 %. Settings stays at about 40 % in every configuration, so it has another limit: the compositor runs at every vblank (4–7 ms), but every other frame is skipped because the previous one has not been presented yet. Settings is the only view with a composited `canvas` layer (layer tree: 17 layers, reasons include `canvas` and `transform3D`); that layer is the next suspect. No view repaints at rest. Timeline: `out/debug/settings-trace.txt` (not in git).

Settings is limited by input, not by frames (WebKit expert, IPC tap on the panel): the view's chart (`ha-chart-base`, zrender) registers a non-passive wheel listener and a `touchmove` handler that takes about 100 ms per event. Over it the scrolling tree asks for synchronous wheel processing, so the wheel events WPE makes from a touch drag went to the busy main thread, the UI process sent the next one only after the answer (3–4 per swipe instead of about 30) and the answers came up to 2 s later; 44 of 64 missed vblanks had no wheel event reaching the web process at all. Two more patches:

- `0009`: the events of a touch-drag gesture (wheel phases `Began` to `Ended`, which on WPE only a touch drag produces) are handled on the scrolling thread also in synchronous wheel regions; the page still receives them but cannot block the scroll. `WALLVIEW_WEBKIT_SYNC_TOUCH_SCROLL=1` restores the old behaviour. With a binary-patched library that forces the same decision: settings 49.7 / 39.6 % → 5.8 / 8.3 %, climate 8.3 / 14.3 % → 9.1 / 8.3 %, lights 3.3 / 2.0 % → 0.6 / 1.3 %. The patch itself is compiled only.
- `0010`: while the page has not answered a touch event, the UI process keeps at most one newer touch move with the same touch points back and sends it with the answer (any other touch event flushes it first), so a slow page is not flooded with 120 moves/s. `WALLVIEW_WEBKIT_NO_TOUCH_COALESCING=1` disables it. Compiled only.

The remaining misses with `0009` are mostly late frames: GPU work that ends after the vblank (hand-over p99 14–16 ms, GPU p99 11–33 ms), led by tile painting on the Skia GPU workers sharing the GPU with the compositor.

Measured with the CI image of run 46014060071 (patches `0001`–`0007`, `0009`, `0010`; the `0005` fix puts the vblank thread on `SCHED_RR` by itself, no `chrt`), same metric, 8 swipes per value:

| Configuration | climate | settings | lights |
|---|---|---|---|
| all patches, 4 rounds | 6.5 / 6.5 / 12.7 / 3.2 (7.2 %) | 9.7 / 11.8 / 3.8 / 7.2 (8.1 %) | 2.6 / 3.9 / 1.3 / 3.3 (2.8 %) |
| `WALLVIEW_WEBKIT_SYNC_TOUCH_SCROLL=1` (no `0009`), 2 rounds | 13.0 / 15.0 (14.0 %) | 42.2 / 19.0 (30.6 %) | 1.3 / 3.3 (2.3 %) |
| `WALLVIEW_WEBKIT_NO_TOUCH_COALESCING=1` (no `0010`), 4 rounds | 9.2 / 3.2 / 4.5 / 1.9 (4.8 %) | 5.2 / 7.8 / 1.3 / 1.9 (4.0 %) | 2.0 / 1.3 / 3.2 / 3.9 (2.6 %) |

`0009` fixes settings as predicted. `0010` does not help the frames and is worse on climate and settings; its effect on how fast the page reacts (the reason for it) was not measured.

What stays on the main thread during a swipe and what it costs (climate, 100 % of one core): every touch move is sent to the page (`WebPageProxy::updateTouchEventTracking()` is `Synchronous` for everything outside Cocoa), i.e. touch and pointer event dispatch with `EventPath` and hit tests, every wheel event is dispatched to the DOM because of the passive `wheel` listener (`dispatchWheelEventViaMainThread()`), and every scroll position runs `RenderLayer::updateLayerPosition` / `recursiveUpdateLayerPositionsAfterScroll`. None of this blocks a frame once 1–4 are fixed, but it produces the tile repaints of cause 3 and keeps one core busy.

## Tried and dropped

- Batching/merging Home Assistant websocket messages and overriding `ha-spinner` in an injected script (overview at rest 6 → 52 fps): removed, because it changes how the page behaves.
- 1.512 GHz CPU: unstable on this unit.
- CPU tile raster with the WPEPlatform build (`WEBKIT_SKIA_ENABLE_CPU_RENDERING=1 WEBKIT_SKIA_CPU_PAINTING_THREADS=3`, with the scrolling-thread change and real-time threads): climate about twice as many missed frames as GPU raster.

## Next steps

1. WebKit with WPEPlatform + custom-property patch + `wallview-browser` (CI build running), then measure real touch scrolling.
2. Expert analysis of the JSC and RenderLayer hot spots (flags, patches, build options).
3. Measurement of what reaches the display (DRM page flips) and touch replay through uinput (kernel option needed).
4. If Chromium stays clearly better: it needs a larger root file system than the 256 MiB slot, i.e. repartitioning the eMMC (owner's decision).

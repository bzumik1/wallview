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

Neither engine holds 60 fps yet; Chromium is close because touch scrolling runs on its compositor thread with GPU raster.

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

# Stock Android reference: what it does for touch and display smoothness

Inventory of everything the panel's original firmware does that affects touch latency, touch smoothness and display or scroll smoothness, and whether WallView can reuse it. Static study of the eMMC backup (read only, nothing run on a panel), 2026-11-27/28.

Stock firmware: Android 8.1.0, build `OPM8.190605.003` (AOSP `android-8.1.0_r65`), Rockchip SDK `RK30_ANDROID8.1.0-SDK-v1.00.00`, product version `NSPanel86P_1.6.5`, vendor kernel Linux 4.4.138 (`SMP PREEMPT`). Board files are the Rockchip `px30_evb` defaults with few changes.

Evidence is cited by image and path inside it: `boot.img` (kernel, its embedded config `IKCFG_ST`, ramdisk `init*.rc`), `system.img`, `vendor.img`, `oem.img`, the decompiled vendor device tree, and the Android kernel and logcat logs from `userdata` (`/data/logs/…`). Symbols come from the kernel's kallsyms and from the libraries' dynamic symbol tables. Values marked *AOSP source* come from the public AOSP 8.1 source, not from the binaries.

Already covered elsewhere and only referenced here:

- Touch controller and the vendor driver: [touch-controller.md](touch-controller.md), "Findings of 2026-11-27" → "Vendor Android driver". Note: the vendor kernel runs at `CONFIG_HZ=300`, so its 300-jiffy ESD cycle is 1.0 s, not the 1.2 s an earlier version of that section derived from 250 Hz.
- AOSP input resampling constants: 5 ms resample latency, 2 ms minimum and 20 ms maximum sample delta, at most 8 ms of prediction, resampling on (`ro.input.noresample` is not set).
- DRAM: the vendor firmware runs the DMC up to 666 MHz; WallView runs it at 333 MHz from the loader (T3's finding; `docs/performance.md` still says 664 MHz and has to be reconciled).

## Summary

Android did not do anything exotic. Its touch path is the AOSP default, its touch and DRAM boosts are kernel input handlers, the power HAL ignores touch, and SurfaceFlinger uses stock vsync offsets. The things worth copying are structural: frames aligned to vblank (input resampling, a queued frame in the compositor), RT priority for the display threads, CPU 3 kept free of background work, and DRAM at 666 MHz. See the ranked list at the end.

## Input

| Item | Android | Evidence | WallView now | Adopt? |
|---|---|---|---|---|
| Touch driver, IRQ | Vendor `hyn_ts`/`cst2xx`, rising edge + oneshot, hard IRQ queues work on a normal-priority workqueue, I²C1 100 kHz | see touch-controller.md | mainline `hynitron_cstxxx` + patches, falling edge, threaded IRQ, 100 kHz | Nothing to port |
| IDC file | None for the touchscreen: `/system/usr/idc` has only `AVRCP.idc`, `qwerty.idc`, `qwerty2.idc`; `vendor.img` has no `usr/idc` | `system.img:/usr/idc/` | – | Nothing: InputReader used its defaults (no `touch.filter`, no size or pressure calibration, device type from `INPUT_PROP_DIRECT`) |
| InputReader / InputDispatcher | Threads at `PRIORITY_URGENT_DISPLAY` (nice −8) under CFS (*AOSP source*, `InputManager.cpp`). No input props set | `system.img`, `vendor.img` `build.prop` | `wallview-touch` is `SCHED_FIFO` 40 (`wallview-touch.c`) | We already go further |
| Batching and resampling | `InputConsumer::consumeBatch` / `resampleTouchState` deliver one resampled move per frame at frame time − 5 ms | `system.img:/lib64/libinput.so` | `wallview-touch` emits a 120 Hz stream from a free-running timer, 8 ms behind, ≤12 ms prediction | **Partly**: drive `wallview-touch` from DRM vblank events and emit one sample per vblank (see ranking, #2). Do not copy the 5 ms latency and 8 ms prediction: a replay of the 2026-11-27 capture with those values gave 1793 stalls and 1142 jerks against 259 and 550 with the current defaults (`docs/performance.md` on `claude/project-thread-brfduo`, 14016d2), because this controller reports far less regularly than Android's assumptions |
| Engine-side scroll resampling | Chromium 131 WebView contains `ResamplingScrollEvents`, `ScrollPredictor::ResampleScrollEvents`, `linear_resampling`, `FilteringScrollPrediction`. In upstream Chromium of that era scroll resampling is on by default (*Chromium source*, not verified in the binary) | `system.img:/app/WebView/WebView.apk`, `lib/*/libwebviewchromium.so` | WPE WebKit scrolls from the raw (already resampled) uinput stream | Covered by the vblank-locked `wallview-touch`; no WebKit change needed |
| Fling velocity | AOSP `VelocityTracker` default strategy `lsq2` (*AOSP source*); Chromium uses its own fling curve on the compositor | – | WPEPlatform gesture controller computes kinetic velocity from our synthetic stream | Check that predicted samples near lift-off do not inflate fling velocity; low priority |
| Kernel input handlers | Two boosts on every `EV_ABS`/`EV_KEY`: `cpufreq_interactive_input_event` (CPU to 1512 MHz for 1.5 s) and `rockchip_dmcfreq_input_event` (DMC to the boost rate, 666 MHz, for 500 ms) | kallsyms: `cpufreq_interactive_input_connect`, `rockchip_dmcfreq_input_connect` | CPU fixed at 1416 MHz, DMC fixed | CPU part: no (1512 MHz unstable on the development unit). DMC part: use a fixed 666 MHz instead |

## CPU and scheduling

| Item | Android | Evidence | WallView now | Adopt? |
|---|---|---|---|---|
| Tick, preemption | `CONFIG_HZ=300`, `CONFIG_PREEMPT=y`, `CONFIG_SCHED_HRTICK=y`, `CONFIG_NO_HZ_IDLE=y` | `boot.img` kernel config | 6.18 EEVDF, `HZ=250`, `PREEMPT` | No: RT frame threads and hrtimers make HZ irrelevant |
| CFS tunables | `sched_latency_ns` 10 ms, `sched_wakeup_granularity_ns` 2 ms, `sched_child_runs_first` 0, `sched_tunable_scaling` 0 | ramdisk `init.rc:123-126` | EEVDF has no such knobs (`base_slice_ns` in debugfs only) | No |
| RT throttling | `sched_rt_runtime_us` 950000 of 1000000 (also on the root `cpuctl` group) | ramdisk `init.rc:132-133,161-162` | Linux default, same value | Same already |
| EAS / schedtune | `CONFIG_SCHED_TUNE=y`, `/dev/stune/{foreground,background,top-app,rt}` created, but EAS turned off: `write /sys/kernel/debug/sched_features NO_ENERGY_AWARE`; `schedtune: init normalization constants...` with no energy model | ramdisk `init.rc:58-79`, `init.rk3326.rc`; kernel log | – | No: schedtune had no effect without EAS and is gone from mainline (uclamp replaces it) |
| cpusets | `top-app` and `foreground` 0-3, `background` and `system-background` **0-2** | ramdisk `init.rk3326.rc` (`on boot`) | All tasks on all CPUs; all device IRQs on CPU 0 | **Yes**: a cgroup v2 cpuset 0-2 for everything except the browser processes, so a frame thread finds CPU 3 idle (#4) |
| CPU governor | `interactive` (`CONFIG_CPU_FREQ_DEFAULT_GOV_INTERACTIVE=y`); power HAL `rk_power_init` writes hispeed 600 MHz; go_hispeed_load 99, timer 20 ms, min_sample_time 40 ms, target_load 90; input boost to 1512 MHz for 1.5 s; `scaling_min_freq` set to 0 at `sys.boot_completed` | kernel config; `vendor.img:/lib64/hw/power.rk3326.so`; ramdisk `init.rk30board.rc:248` | `performance`, 1416 MHz fixed | No: we are already always at the top stable clock |
| UI thread priority | No `sys.use_fifo_ui`, so app UI and RenderThread stay at nice −10/−4 under CFS. SurfaceFlinger itself is RT: main thread, `DispSyncThread`, `EventThread`, `SFEventThread` are `SCHED_FIFO` (priority 2 in *AOSP source*) | `system.img:/bin/surfaceflinger` and `/lib64/libsurfaceflinger.so` strings `Couldn't set SCHED_FIFO for DispSyncThread/EventThread/SFEventThread`, `… on display on` | WebKit frame threads RT (`SCHED_RR` 5, per the 2026-11-27 study; not found on `main`), `wallview-touch` `SCHED_FIFO` 40 | Keep; check that the thread that commits to DRM (WPEPlatform DRM view in the UI process) is also RT, because that is SurfaceFlinger's role |
| Writeback | `vm.dirty_background_ratio` 1, `vm.dirty_ratio` 3, swappiness 100 with zram lz4 | ramdisk `init.rk3326.rc` | Defaults | Optional: small dirty ratios avoid writeback bursts during log writes to `/data`; negligible for a mostly read-only rootfs |
| Power HAL | INTERACTION and LAUNCH hints ignored; LOW_POWER writes `l`/`L` to `dmc/system_status`; SUSTAINED_PERFORMANCE sets CPU and GPU governors to `performance` and DMC to `p` | `power.rk3326.so` (`rk_power_hint`) | – | Nothing to port |
| Per-app performance mode | `oem.img:/etc/package_performance.xml` lists benchmarks and a few third-party apps with `mode="1"`; neither eWeLink nor HA Companion is listed | – | – | Nothing |

## Memory and DDR

| Item | Android | Evidence | WallView now | Adopt? |
|---|---|---|---|---|
| DMC OPPs | 194, 328, 450, 528, 666 MHz | vendor DT `dmc-opp-table` | Fixed rate from the loader | – |
| DMC policy | `auto-freq-en=1`, `auto-min-freq` 328 MHz, upthreshold 40, downdifferential 20 (load from DFI); `system-status-freq` NORMAL/BOOST/PERFORMANCE 666, REBOOT/VIDEO_1080P 450, SUSPEND 194 MHz | vendor DT `dmc` node | – | Fixed 666 MHz. Android ran there whenever a finger was down (input boost) and under load |
| VOP bandwidth → DMC | Not active: `rockchip-dmc dmc: failed to get vop bandwidth to dmc rate`, `rockchip-drm display-subsystem: devfreq is not set`; HWC computes `vop_bw` but fails to open `/sys/class/devfreq/dmc/vop_bandwidth` | Android kernel log; `hwcomposer.rk30board.so` strings | – | Nothing |
| Cost of scaling | Each Rockchip DDR rate change is a firmware (SIP) call that stalls memory traffic; with load-based scaling between 328 and 666 MHz Android paid that during interaction | inferred from the policy above | No scaling | Keep the rate fixed; do not port load-based DMC scaling |

## GPU

| Item | Android | Evidence | WallView now | Adopt? |
|---|---|---|---|---|
| Driver | Mali Bifrost kbase built in (`CONFIG_MALI_BIFROST=y`, `…_DEVFREQ=y`, with `…_DEBUG=y` and `…_FENCE_DEBUG=y`), user space DDK r12p0; `GPU identified as 0x3 arch 7.0.9 r0p0`, `Probed as mali0` | kernel config, kernel log | Panfrost + Mesa | No (large out-of-tree port; Android's driver was a debug build) |
| DVFS | devfreq `simple_ondemand` on 200/300/400/480 MHz; `performance` only for SUSTAINED_PERFORMANCE; thermal power model `mali-simple-power-model` | vendor DT `gpu-opp-table`, `power.rk3326.so`; ramdisk `init.rk3326.rc` chowns `ff400000.gpu/governor` | 480 MHz fixed (lower OPPs rejected by the regulator) | No: a ramping GPU can only be slower at the start of a scroll |
| Power policy | Default `demand` (first entry of kbase `policy_list`: `kbase_pm_demand_policy_ops`, then `coarse_demand`, `always_on`); `power-off-delay-ms not available` (driver default) | kallsyms, kernel log | Panfrost runtime PM with autosuspend | No measurable difference expected; if first-frame-after-idle latency ever shows up, raise Panfrost's autosuspend delay |
| Buffers / AFBC | HWC has AFBC paths (`Plane(%d) cann't support afbc`, `sys.gmali.fbdc_target`), the kernel logged `win[2] … not support afbdc`; whether app buffers were compressed is not established | `hwcomposer.rk30board.so`, kernel log | Linear buffers; mainline PX30 VOP has no AFBC | No |
| HWC CPU/GPU control | `ctl_cpu_performance`, `ctl_gpu_performance`, `set_cpu_min_freq` exist but nothing in the library calls the first two; `set_cpu_min_freq` is used only by `ctl_little_cpu` | `objdump` call graph of `hwcomposer.rk30board.so` | – | Nothing |

## Display and composition

| Item | Android | Evidence | WallView now | Adopt? |
|---|---|---|---|---|
| Timing | 480×480, 16 MHz pixel clock, h 12/10/15, v 12/8/15 → 60.09 Hz, MIPI DSI 2 lanes | vendor DT `display-timings` | Same timing | – |
| Density | `ro.sf.lcd_density=160` → DPR 1, so the dashboard was laid out at 480 CSS px | `vendor.img:/build.prop` | Page zoom 1.5 (320 CSS px) | Use zoom 1.0 for A/B comparisons with Android |
| Composition | `sys.hwc.compose_policy=6`; drm HWC scans the app layer out on a VOP plane; after a static period it switches to GLES composition (`hwc_static_screen_opt_set`, `Enter static screen opt`) for power | `vendor.img:/build.prop`, HWC strings | Direct scanout of the browser buffer | Nothing |
| Vsync source | HWC `VSyncWorker` (`drmWaitVBlank`, `GetPhasedVSync`, `SyntheticWaitVBlank` fallback) feeds SurfaceFlinger's DispSync; app and SF phase offsets 1 ms | HWC symbols; logcat `ISurfaceFlingerConfigs` | WebKit paces on page-flip events | The vblank timestamps are the clock to lock `wallview-touch` to (#2) |
| Buffering | Triple buffering on (`Disabling Triple Buffering` exists but is not logged); `debug.sf.disable_backpressure` set at boot (`Disabling backpressure propagation` logged), so SF latches the newest ready buffer; `maxFrameBufferAcquiredBuffers` 2/3 | `libsurfaceflinger.so` strings, logcat | One frame in flight: the next frame starts only after the previous page flip | **Yes**: queue one finished frame in the WPEPlatform DRM view (#1) |
| Other SF props | `debug.sf.latch_unsignaled`, `persist.sys.refresh_skip_count` (Rockchip) exist but are not set in any prop file | `libsurfaceflinger.so` strings, `build.prop` | – | Nothing |
| HWUI caches | `ro.hwui.*` cache sizes (texture 72 MB, layer 48 MB …) | `vendor.img:/build.prop` | – | Not applicable: they apply to Android views, not to the WebView's Chromium compositor |

## Other

| Item | Android | Evidence | WallView now | Adopt? |
|---|---|---|---|---|
| Web engine | Chromium 131 WebView, renderer **32-bit** (`use32bitAbi`, `webview_zygote32`), scrolling on the compositor thread | `WebView.apk`, logcat | WPE WebKit 2.50, 64-bit | The largest difference, already measured in `docs/performance.md` (Chromium on our kernel is smoother); a decision for the owner, not a tuning |
| Thermal | `soc-thermal`: passive trips 70 °C and 85 °C, critical 115 °C, `power_allocator` (`sustainable-power` 750 mW), CPU and GPU as cooling devices; tsadc hardware shutdown 120 °C; idle 43–47 °C | vendor DT `thermal-zones`, `tsadc` | About 50 °C under load, no throttling | Nothing: neither system throttles |
| Kernel build | `CC_OPTIMIZE_FOR_SIZE=y`, `MALI_BIFROST_DEBUG=y`, `ROCKCHIP_DRM_DEBUG=y`, `SCHED_DEBUG=y`, `FTRACE=y` | kernel config | – | Android was handicapped here, not helped |

## Candidates, ranked by expected effect on touch and scroll smoothness

1. **Queue one frame in the WPEPlatform DRM view.** Android keeps a buffer queue and SurfaceFlinger latches the newest ready buffer; WallView starts the next frame only after the previous page flip, so one late frame costs two vblanks. Cost: up to one frame of latency. Risk: low (user space, switchable).
2. **Lock `wallview-touch` to vblank.** One resampled sample per vblank, timed from the DRM vblank, as AOSP `InputConsumer` does. Removes the 1/2/3-moves-per-frame judder of the free-running 120 Hz timer. Keep `wallview-touch`'s own latency and prediction: Android's 5 ms and 8 ms replayed much worse on this controller (1793 stalls and 1142 jerks against 259 and 550). Risk: low.
3. **DRAM at a fixed 666 MHz.** Android ran there during every touch and under load. Risk: low to medium (vendor BL31 path, must be stress-tested on the unit).
4. **Keep CPU 3 for the browser.** Cpuset 0-2 for every non-browser process, like Android's `background` and `system-background`. Risk: low.
5. **RT priority for the DRM commit thread.** SurfaceFlinger, DispSync and its event threads were `SCHED_FIFO` on Android. Check the thread that commits page flips in the UI process and give it RT priority below `wallview-touch`. Risk: low.

Not worth porting: the 1512 MHz CPU touch boost (unstable on the development unit), the interactive governor, load-based DMC and GPU scaling, schedtune, HZ 300, the power HAL, the Mali DDK.

## Not established

- Whether Android's gralloc allocated AFBC buffers for the WebView.
- Chromium 131's runtime defaults for `ResamplingScrollEvents` and the scroll predictor on this device (only the feature names are confirmed in the binary).
- No frame statistics (gfxinfo, systrace) exist in the backup, so Android's smoothness cannot be quantified from it.

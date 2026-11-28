# wallview-touch audit (origin/claude/project-thread-brfduo @ c975e3d), 2026-11-28

Read-only audit by an independent reviewer (Fable 5.2). Offline test and replay were built with
gcc 13 in the Lima VM (in the VM's own /tmp, removed afterwards). Nothing ran on the panel, so all
CPU figures for the PX30 are estimates; replay figures are measured.

File:line refers to `buildroot/board/wallview/touch/wallview-touch.c` unless stated.

## 0. What the capture says (touch-raw-2026-11-27.txt)

- 2086 reads: 1940 pressed (`1 3`), 81 unpressed (`1 0`), 65 empty (`0 0`).
- Every lift, real or false, starts with exactly one unpressed read about 22 ms after the last pressed one.
  - 65 times it is followed by an empty read 28-54 ms later (two outliers 74 and 123 ms): real lift, or a false
    lift that goes through "empty" (5 cases, re-press 36-72 ms later).
  - 16 times it is followed by a pressed read 35-104 ms later: false lift (what patch 0006 bridges).
- Before a false lift the coordinates freeze (several identical reads), and the contact re-appears
  57-98 px away, mostly ahead on the stroke. The fitted lift speed (250-500 px/s) therefore
  underestimates where the finger lands.
- Coordinate changes while pressed: p10 21.7, p50 38.6, p90 87.7 ms. One read takes 3.74 ms (p50).

## 1. Correctness

### 1.1 HIGH - lift hold destroys the release velocity of every flick (design, measured)
`coast()` 591-606, hold 563-567. During the 100 ms hold the output coasts with an exponential stop capped
at 40 px, then the up is sent. Measured on the 26 flicks with raw lift speed > 300 px/s
(instrumented replay, output speed just before the up, as a fraction of the raw lift speed):

| configuration | last 50 ms | last 150 ms | flicks with no motion in last 50 ms | output downs |
|---|---|---|---|---|
| defaults | 0.06 | 0.50 | 13 / 26 | 76 |
| defaults + patch 0006 model | 0.00 | 0.04 | 26 / 26 | 64 |
| LIFT_MS=0 | 1.25 | 1.00 | 0 | 81 |
| patch 0006 model + LIFT_MS=0 | 0.67 | 0.92 | 0 | 65 |
| COAST_MS=400 COAST_PX=150 | 0.73 | 0.88 | 1 | 76 |
| LIFT_MS=80 COAST_MS=150 COAST_PX=80 REJOIN_PX=60 | 0.58 | 0.77 | 3 | 70 |

A browser that derives fling velocity from the last events before touch-up (WebKit's kinetic scroll
uses a window of about 150 ms, from memory, not checked in the tree) sees half the speed today and
almost none with 0006. Fix: either keep velocity through the hold (near-linear coast: long time
constant, cap by distance only) or do not hold when 0006 is active. Confidence: replay numbers high;
effect in WPE medium, verify with `list.html` fling distance on the panel.

### 1.2 HIGH - patch 0006 and the daemon's hold stack, and 0006 disables the daemon's gates
`kernel/patches/0006-...patch` (CST3XX_MAX_UNPRESSED_FRAMES 2, CST3XX_KEEP_TIMEOUT_MS 60), daemon 491, 596.
- With 0006 every real lift, taps and long presses included, is reported at the empty read or the 60 ms
  timeout: +28-54 ms (p50 about 45). The replay's "tap release delay" does not show it, because it
  measures from the already delayed lift.
- The lift then arrives >= 65 ms after the last coordinate change, so `coast_after` (40 ms, line 596)
  never allows coasting, and `hold_recent` (100 ms, line 491) often refuses the hold. During the kept
  frames `predict()` fades back to the last sample. Result: output frozen for 45 ms + 100 ms before the up.
- Replay with 0006 model: defaults 64 downs, release p50 53 / p95 155 / max 181 ms; LIFT_MS=0 gives 65 downs,
  46 / 54 / 123 ms. The daemon's hold buys one bridged lift for 100 ms on every moving lift.
Fix: with 0006 deployed set LIFT_MS=0 (or make it the default when the kernel keeps contacts), or drop
0006 and bridge in the daemon only (see 4). Confidence high (replay), model caveat in 4.3.

### 1.3 MEDIUM (rare, severe) - SYN_DROPPED resync invents contacts in slots 5-9
`read_slots()` 701-709, `resync()` 740-752. `slots` is zero-initialised and EVIOCGMTSLOTS writes only
`num_slots` values (5 for the CST226SE, `max_touch_num = 5` in patch 0002). Slots 5-9 keep tracking id 0,
which is >= 0, so each is "started". The output device has ABS_MT_SLOT max 4, the kernel ignores the slot
change, and the tracking id, major and position 0,0 land in whichever slot was current: a stuck or
corrupted touch, and five contacts that never end, so the timer runs forever.
Fix: fill `values` with -1 before the ioctl, or remember the device slot count (ABS_MT_SLOT max + 1) and
loop only over it everywhere. Needs a daemon stall of roughly a second with a moving finger to trigger.
Confidence high from code reading (not reproduced: no evdev device in the harness).

### 1.4 MEDIUM - rejoin gate too tight for how this controller re-acquires
`rejoins()` 472. Of 13 held false lifts that came back in time, 8 were rejected with
"off 57-70 px, limit 41-61 px". See 4 for the numbers; REJOIN_PX=60 bridges them.

### 1.5 MEDIUM - prediction is retracted: 152 back-steps (>= 2 px reversals, 1928 px in total)
`predict()` 623-627 pulls the output back to the last sample after 1.5 intervals, and the next sample
then restarts from an interpolated point behind the predicted one. PREDICTION_MS=0 gives 0 back-steps
(but 1049 stalls). Scratch variant "blend offset on every new sample + no fade" gives 9 back-steps and
max step 51 px (104 now), at the price of stalls 679 (259 now) and path error p50 9.9 px (7.0 now).
Not a clear win; an option to A/B on the panel, not a default change. Confidence high (measured).

### 1.6 LOW - vblank mode: one or two wakeups after the last finger left
Main loop 1027-1039. A vblank event queued before the lift still arrives; it arms the phase timer and
runs `resample()` with no contact. Fix: when `first_active() < 0`, read the event and do nothing.
Then idle is exactly zero wakeups (it already is in timer mode: `set_timer(0)`, 320).

### 1.7 LOW - a busy loop is possible at SCHED_FIFO 40
`read_vblank()` 331: a `read()` error other than EAGAIN leaves the fd readable and the loop spins.
Fix: on such an error call `close_drm()`. Same hardening for unexpected errors on the timer fds.

### 1.8 LOW - first move can step backwards
Resample 651-661: when the target time is older than the oldest sample in history, the fallback is the
newest sample; the next tick then interpolates from the older one. Fix: fall back to `history[0]`.
Needs two samples within 16 ms of the touch-down, rare with this controller.

### 1.9 LOW - ABS_MT_TOUCH_MAJOR is sent only at touch-down (550); stale afterwards and after a rejoin.
Either forward changes or do not advertise the axis.

### 1.10 LOW - startup and options
- 964-969: a touch that starts between the BTN_TOUCH check and the drain loses its down and is ignored
  until lift (also the "grabbing anyway" case). Fix: build state with the resync path after the grab.
- 981: `sched_setscheduler` result ignored; no `mlockall` (see 2).
- 895, 901, 909...: `(int)` and `int64_t` casts of unbounded doubles are undefined for huge values;
  RATE outside 2-1000 and BLEND > 0.95 are replaced silently. Clamp and log.
- 815-838: uinput ioctl results unchecked. Properties themselves are right: INPUT_PROP_DIRECT,
  BTN_TOUCH, ABS ranges and resolution copied from the source, slot max clamped, ABS_X/ABS_Y follow
  the first active slot.
- Signals: none handled; default termination closes the grab and destroys the uinput device, which is
  sufficient.
- DRM safety (227-280): scan, driver check and DROP_MASTER probe are sound. Remaining window: the browser
  exits between scan and open, the daemon becomes master for microseconds, drops it and closes.

## 2. Performance and efficiency (estimates for the PX30; compute measured in the VM)

- Compute: one tick plus a third of an input frame is about 0.1 µs on the VM's core including a write
  to /dev/null; expect 1-2 µs on a Cortex-A35. No allocation, no stdio outside debug. Irrelevant next
  to the system calls.
- Timer mode, per tick: `poll`, `read(timerfd)`, `write(uinput)` when the position changed = 3 system
  calls, about 25-50 µs with the reader wakeup; at 120 Hz about 0.3-0.6 % of one core.
  Per input frame: `poll` + 2 `read` (the second only returns EAGAIN; one read of 64 events is enough).
- Vblank mode, per frame: 2 `poll`, 2 `read(drm)`, `timerfd_settime` (watchdog, 349), `timerfd_settime`
  (phase), `read(phase)`, `write`, `ioctl(WAIT_VBLANK)` = 9 system calls and 2 wakeups per frame.
  Cheaper: watchdog as a `poll` timeout instead of re-arming a timerfd every frame, single read; that is
  6 calls. Further: run the phase timer free at the measured frame length and ask for a vblank event
  only every N frames to correct the phase (1 wakeup per frame).
- Resting finger: the timer keeps ticking at 120 Hz with no output for the whole long press
  (578, 692). Fix: disarm when every contact is settled (no offset, not releasing, target past the
  last sample and the prediction faded), re-arm in `handle_frame`.
- Idle: zero wakeups in timer mode; vblank mode after the fix in 1.6.
- Downstream cost is the real one: 120 Hz output makes the browser handle two touch moves per 60 Hz
  frame. RATE=60 in replay: stalls 119 (259), jerks 602 (550), same path error. Display-locked 60 Hz
  halves the browser's touch work and removes the 120-vs-60 Hz beat.
- `/proc` scan (187-224, called from 244 via `arm_timer` 311): runs at SCHED_FIFO 40 right after the
  first touch-down is flushed, then only on failure every 5-60 s while a finger is down; never again
  once the node is open. VM: 1.6 ms for 139 processes and 564 fds; expect roughly 8-25 ms on the panel
  (the debug line prints the real value). Fix: scan at start and on release, never while a contact is
  active; or read `/sys/kernel/debug/dri/0/clients` if debugfs is mounted (one file, shows the master).
- RT hygiene: add `mlockall(MCL_CURRENT | MCL_FUTURE)` before the loop so a page of the binary evicted
  under memory pressure cannot cost a flash read on the first touch after idle; warn if
  `sched_setscheduler` fails.

## 3. Latency budget (added by design)

| stage | added |
|---|---|
| I2C read before the event is timestamped (i2c1 has no `clock-frequency`, so 100 kHz; `kernel/dts/px30-itead-nspanel-pro.dts` &i2c1) | 3.7 ms |
| touch-down | 0 |
| moves, timer mode | 16 ms + 0-8.3 ms tick phase (20 ms mean, beating against the display) |
| moves, vblank mode, offset 10 | 16 ms + 6.7 ms to the next vblank = 22.7 ms, constant |
| tap / resting lift | 0 (with 0006: +28-54 ms) |
| lift of a moving finger | 100 ms + up to one tick (with 0006: about +45 ms more; max 181 ms in replay) |

Reductions that do not cost smoothness:
- `clock-frequency = <400000>` on i2c1: about -2.8 ms on every sample, and tighter timestamps. Kernel/DT
  change: needs the reviewer and a panel test (pull-ups).
- Vblank offset 10 -> 13-14 ms: -3 to -4 ms, same interpolation; verify on the panel that the browser
  still picks the sample up before it starts the frame.
- Release path as in 1.1/1.2 (the largest item by far).
LATENCY_MS itself should stay: 12 gives path error p50 5.9 (7.0) but jerks 633 (550) and stalls 306 (259);
8 gives 742 jerks. 24 gives jerks 423, stalls 207, max step 80, at +8 ms.

## 4. Replay

### 4.1 Current defaults (matches docs/performance.md)
Full capture: raw 81 downs, 21 within 200 ms of an up -> 76 downs; path error p50 7.0 / p95 32.1 / max
121.5 px; max step 104 px; jerks 550; stalls 259; release after last sample p50 27 / p95 131 / max 134
ms; tap release delay 0 (n 36). Partial capture: 16 -> 16 downs, error 7.1 / 22.4, jerks 304, stalls 72.
With 0006 model: 65 raw -> 64 downs, jerks 659, stalls 291, release 53 / 155 / 181 ms.
Synthetic test: all 12 cases pass.

### 4.2 Parameter changes the replay supports
- `WALLVIEW_TOUCH_REJOIN_PX` 30 -> 60: 76 -> 68 downs (13 instead of 5 false lifts bridged), partial
  capture 16 -> 14 (both short gaps), path error 7.2 / 33.8, jerks 575, stalls and release unchanged,
  tap delay 0. No further gain at 75-120. REJOIN_SLACK 3 gives the same 68. A rejoin can only happen in
  the same slot within the hold of a moving finger, so the wider radius is low risk; the two synthetic
  cases "second finger sideways" (55 px) and "touch behind stroke" (45 px) would need their distances
  raised or a forward-only widening (along the stroke up to 90 px, sideways and behind unchanged).
- Coast that keeps speed (COAST_MS 400, COAST_PX 150) or a shorter hold (LIFT_MS 80: 70 downs with
  REJOIN_PX 60, release p95 110 ms): see table in 1.1. Overshoot of a real lift grows with COAST_PX,
  which matters for sliders; decide on the panel.
- With 0006: LIFT_MS=0 (1.2).
- Not supported: lowering HOLD_PX_PER_S (30 or 0: no extra bridge, tap release delay max 100 ms);
  HOLD_RECENT_MS 150 (one more bridge without 0006); LIFT_MS 120-150 (66-74 downs, release p95 149-181).

### 4.3 Limits of the replay tool (wallview-touch-replay.c)
- No release-velocity metric; this hid 1.1.
- "tap release delay" is measured from the modelled kernel lift, so 0006's delay is invisible (107-108).
- REPLAY_KEEP_UNPRESSED keeps any number of unpressed reads and has no 60 ms timeout (237-241); equal to
  the patch on this capture (all runs are one read) except the two lifts whose empty read came after
  74 and 123 ms.
- Samples are stamped with the read start; the kernel stamps after the read (+3.7 ms).
- Jerks are per tick^2, so runs at different RATE are not comparable.

## Init script (S35wallview-touch)
No defect found. `touch-resampling = off` is only read between restarts, and the log is trimmed only at
a restart (debug mode grows it in tmpfs meanwhile).

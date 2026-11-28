# wallview-touch and kernel patch 0006: review loop

Jakub (2026-11-28): two independent touch-input reviewers, one on Fable 5.2 and one on Opus 5.5, review `wallview-touch` and kernel patch 0006 in rounds until they stop finding bugs. Each round reviews one commit of `claude/project-thread-brfduo` (PR #22), read-only, with the offline test and the replay of the 2026-11-27 captures (`tools/perf/captures/`) built normally and with `-fsanitize=address,undefined`. The thread that owns PR #22 fixes the findings; the next round reviews its fix commit.

## Round 1: 702c59f

Both reviewers: offline test passes, clean under ASan/UBSan and `-Wall -Wextra`; replay clean on both captures; independent five-slot protocol fuzzers (Opus 3 × 400k steps, Fable 3 × 60k frames) found no violations (down/up pairing, `BTN_TOUCH`, ranges, stuck contacts, timer left armed). Not executed by either: the vblank/DRM path, resync ioctls and signals on hardware.

### High

- **0006 timeout too short (Opus H1, Fable M5 via the replay).** `CST3XX_KEEP_TIMEOUT_MS` 60 (patch, around line 40; work at 116-122, 198-200). After the single unpressed read the controller sends no interrupt until the finger presses again; 7 of the 16 false lifts in the capture come back 62–104 ms later, so the kept contact is already released and the re-press is a new touch. An exact model of the patch (at most two unpressed reads, then the timeout) gives 69 output downs at 60 ms, 66 at 85 ms, 64 at 110 ms; real lifts end with an empty read 27–54 ms after the unpressed one (63 of 65, outliers 74 and 123 ms). Fix: 110–120 ms. Consequence: `docs/performance.md`'s "64 downs with 0006" is wrong for the shipped 60 ms (69).

### Medium

- **Replay model of 0006 (both).** `wallview-touch-replay.c` 237-241: `REPLAY_KEEP_UNPRESSED` keeps any number of unpressed reads with no timeout; "release after last touch sample" is measured from the kept duplicate and "tap release delay" from the already delayed lift, which hides the 27–54 ms 0006 adds to every tap. Fix: model the kernel exactly (two reads, timeout) and measure from the last pressed read.
- **Held lift that may not coast snaps back and freezes (Fable).** `wallview-touch.c` 613-615, 707-711, 580-585: the hold accepts lifts up to 100 ms old and from 60 px/s, coast only up to 85 ms and from 150 px/s. Reproduced: 500 px/s, lift 92 ms after the last sample, output 235 → 220, standstill 80 ms, release velocity 0; every 60–150 px/s drag end (sliders) steps back and waits 80 ms. Fix: keep the emitted position when coast declines, or do not hold then.
- **Prediction retracts into a late lift (Fable).** 660-664: lifts 100–133 ms after the last sample release during the fade, 7 px backwards, last-50-ms velocity −0.48 of the stroke speed (2 of 30 moving lifts). Fix: fade by freezing, not retracting.
- **With 0006 the output stalls before every moving lift (Fable).** 649, 658-659: the 30 ms prediction cap is shorter than the kernel's 50–78 ms lift delay; 12 of 27 held lifts stand still ≥ 17 ms before the coast starts (0 without the patch). Fix: continue on the coast curve once the cap is reached.
- **0006 has no distance gate (Fable).** Patch 58-71, 179-182: the same finger id pressed again within two reads or 60 ms continues the contact wherever it lands, so a second finger or a fast double tap becomes one jumping contact that the daemon interpolates into a swipe. Fix: beyond about 150 px release and restart the slot. Needs a capture to confirm.

### Low

- 0006: release work on `system_percpu_wq` (normal kworker) while the IRQ thread is FIFO 50; use `system_highpri_wq` (Opus).
- 0006: `kept_at` shared by all slots, a second finger's unpressed read extends the first finger's timeout; make it per slot (Opus).
- 0006: error paths drop kept contacts and sync a half-built frame (Fable, patch 142-174).
- Vblank mode: the vblank queued before the last lift still wakes the daemon twice (both; earlier audit 1.6).
- Vblank mode: losing the DRM fd mid-touch stops output for about 50 ms (Fable, 357-360); with the display off the failing `WAIT_VBLANK` is retried every tick and input frame (Opus, 319); WAIT_VBLANK always uses CRTC 0 (Opus 308; Fable checked that only `vopb` is enabled, so correct today).
- `open_drm` retries silently forever after finding itself master; backoff reaches 80 s, not the documented 60 s (both, 244 / 272-275).
- In vblank mode the `/proc` scan runs at FIFO 40 right after a lift, delaying a double tap's second down (Opus, 1166).
- One slot ending and another starting in the same frame sends `BTN_TOUCH` 0 then 1; queue it once in `flush()` (Opus, 468 + 572).
- `ABS_X`/`ABS_Y` jump to the lowest slot instead of staying on the oldest contact (Fable, 162, 572).
- A touch during startup is lost until lift, and grabbing after 30 s leaves the browser a stuck contact; call `resync()` (both, 1037-1054; earlier audit 1.10).
- `ABS_MT_TOUCH_MAJOR` sent only at touch-down (Opus, 567; earlier audit 1.9).
- Pressure-only frames re-arm the timer for a resting finger: 75 extra wakeups in 2 s (Fable, 596).
- Hold and coast also apply to one finger of a pinch: up to 80 px zoom overshoot, two fingers seen 80 ms longer; do not hold while another slot is active (Opus, design).
- `MAX_SLOTS` 10 not tied to the device's 5 slots (Fable, 23, 820); `ease_from` negative when `MAX_GAP_MS` < 33 ms (Fable, 652-653).
- `S35wallview-touch` trims the log only at a restart, so with `DEBUG` it grows in RAM (Opus).

Checked without defect by both: slot and tracking-id handling, `SYN_DROPPED` drain and resync, rejoin/coast/blend arithmetic (no undefined behaviour up to 1e6 settings), uinput properties, signals, DRM master gate, kernel locking, work and IRQ teardown order, init script.

Verdict round 1: 1 high (0006 timeout), 5 medium. Not done.

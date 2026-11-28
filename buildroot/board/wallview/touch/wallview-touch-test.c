#define _GNU_SOURCE
#include <stdarg.h>
#include <sys/ioctl.h>
static int fake_ioctl(int fd, unsigned long request, ...);
#define main wt_main
#define ioctl fake_ioctl
#include "wallview-touch.c"
#undef ioctl
#undef main
#include <assert.h>
static int kernel_id[MAX_SLOTS], kernel_x[MAX_SLOTS], kernel_y[MAX_SLOTS], slot_reads, move_after = -1, moved_x, moved_y;
static int fake_ioctl(int fd, unsigned long request, ...) {
	va_list ap; va_start(ap, request); void *arg = va_arg(ap, void *); va_end(ap);
	if (fd < 0) return -1;
	if (request == EVIOCGABS(ABS_MT_SLOT)) { ((struct input_absinfo *)arg)->value = 0; return 0; }
	if (_IOC_TYPE(request) == 'E' && _IOC_NR(request) == 0x0a) {
		uint32_t *code = arg; int32_t *v = (int32_t *)(code + 1);
		if (slot_reads++ == move_after) { kernel_x[0] = moved_x; kernel_y[0] = moved_y; }
		for (int i = 0; i < MAX_SLOTS; i++) v[i] = *code == ABS_MT_TRACKING_ID ? kernel_id[i] : *code == ABS_MT_POSITION_X ? kernel_x[i] : kernel_y[i];
		return 0;
	}
	return ioctl(fd, request, arg);
}
#define MS NSEC_PER_MSEC
static int pipefd[2];
static struct options o;
static int slot;
static int downs, ups, cur_slot, outx[MAX_SLOTS], outy[MAX_SLOTS], down_now[MAX_SLOTS];
static double maxstep;
static int64_t maxstill, still_since;
static int lastx = -1, lasty = -1, abs_x = -1;
static void ev(int64_t t, int type, int code, int value) { struct input_event e = { .type = type, .code = code, .value = value }; e.input_event_sec = t / NSEC_PER_SEC; e.input_event_usec = (t % NSEC_PER_SEC) / 1000; handle_event(&e, &slot); }
static void s(int64_t t, int sl) { ev(t, EV_ABS, ABS_MT_SLOT, sl); }
static void down(int64_t t, int sl, int id, int x, int y) { s(t, sl); ev(t, EV_ABS, ABS_MT_TRACKING_ID, id); ev(t, EV_ABS, ABS_MT_POSITION_X, x); ev(t, EV_ABS, ABS_MT_POSITION_Y, y); ev(t, EV_SYN, SYN_REPORT, 0); }
static void move(int64_t t, int sl, int x, int y) { s(t, sl); ev(t, EV_ABS, ABS_MT_POSITION_X, x); ev(t, EV_ABS, ABS_MT_POSITION_Y, y); ev(t, EV_SYN, SYN_REPORT, 0); }
static void up(int64_t t, int sl) { s(t, sl); ev(t, EV_ABS, ABS_MT_TRACKING_ID, -1); ev(t, EV_SYN, SYN_REPORT, 0); }
static struct input_event frame_events[1024];
static int frame_len;
static int mixed_frames;
static void apply_frame(void) {
	int ending[MAX_SLOTS] = { 0 }, touched[MAX_SLOTS] = { 0 }, sl = cur_slot, transition = 0, slots = 0;
	for (int i = 0; i < frame_len; i++) {
		struct input_event *e = &frame_events[i];
		if (e->type != EV_ABS) continue;
		if (e->code == ABS_MT_SLOT) { sl = e->value; continue; }
		if (e->code == ABS_X || e->code == ABS_Y || sl < 0 || sl >= MAX_SLOTS) continue;
		if (!touched[sl]) { touched[sl] = 1; slots++; }
		if (e->code == ABS_MT_TRACKING_ID) { transition = 1; if (e->value < 0) ending[sl] = 1; }
	}
	if (transition && slots > 1) mixed_frames++;
	for (int i = 0; i < frame_len; i++) {
		struct input_event *e = &frame_events[i];
		if (e->type != EV_ABS) continue;
		if (e->code == ABS_MT_SLOT) cur_slot = e->value;
		else if (e->code == ABS_MT_TRACKING_ID) { if (e->value < 0) { ups++; down_now[cur_slot] = 0; } else { downs++; down_now[cur_slot] = 1; lastx = -1; } }
		else if (e->code == ABS_MT_POSITION_X && !ending[cur_slot]) outx[cur_slot] = e->value;
		else if (e->code == ABS_MT_POSITION_Y && !ending[cur_slot]) outy[cur_slot] = e->value;
		else if (e->code == ABS_X) abs_x = e->value;
	}
	frame_len = 0;
}
static void drain(int64_t t) {
	struct input_event e[256]; ssize_t n;
	while ((n = read(pipefd[0], e, sizeof e)) > 0)
		for (size_t i = 0; i < (size_t)n / sizeof e[0]; i++) {
			assert(frame_len < 1024);
			frame_events[frame_len++] = e[i];
			if (e[i].type == EV_SYN && e[i].code == SYN_REPORT) apply_frame();
		}
	if (down_now[0]) {
		if (lastx >= 0) {
			double d = hypot(outx[0] - lastx, outy[0] - lasty);
			if (d > maxstep) maxstep = d;
			if (d == 0) { if (!still_since) still_since = t; if (t - still_since > maxstill) maxstill = t - still_since; } else still_since = 0;
		}
		lastx = outx[0]; lasty = outy[0];
	}
}
static int64_t clock_t0;
static void run_until(int64_t t) { while (clock_t0 + o.period <= t) { clock_t0 += o.period; if (timer_armed) resample(&o, clock_t0); drain(clock_t0); } }
static void reset(void) { memset(contacts, 0, sizeof contacts); downs = ups = 0; maxstep = 0; maxstill = 0; still_since = 0; lastx = -1; memset(down_now, 0, sizeof down_now); }
static void defaults(void) {
	setenv("WALLVIEW_TOUCH_LIFT_MS", "", 1);
	o = (struct options){ .period = NSEC_PER_SEC / 120, .latency = env_ms("WALLVIEW_TOUCH_LATENCY_MS", 16), .max_prediction = 30 * MS, .max_sample_gap = 150 * MS,
		.lift_hold = env_ms("WALLVIEW_TOUCH_LIFT_MS", 80), .rejoin_min = 30, .rejoin_slack = 0.75, .hold_travel = 13, .start_travel = 13, .hold_speed = 60, .start_speed = 150,
		.hold_recent = 100 * MS, .coast = 250 * MS, .coast_after = 85 * MS, .coast_max = 80, .coast_speed = 150, .blend = 0.7, .ease_min = 100 * MS, .gap_blend = 280 * MS, .gap_blend_min = 60 * MS, .predict_travel = 13, .debug = getenv("DBG") != NULL };
}
static int64_t drag(int64_t t, int sl, double *x, double *y, double vx, double vy, int n, int64_t dt) {
	for (int i = 0; i < n; i++) { t += dt; run_until(t); *x += vx * dt / NSEC_PER_SEC; *y += vy * dt / NSEC_PER_SEC; move(t, sl, (int)*x, (int)*y); drain(t); }
	return t;
}
int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); if (pipe(pipefd) < 0) return 1; fcntl(pipefd[0], F_SETFL, O_NONBLOCK); output = pipefd[1];
	timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK); defaults(); period = o.period; opts = &o;
	assert(o.lift_hold == 80 * MS);
	{ struct options d; load_options(&d);
	  assert(d.period == o.period && d.latency == o.latency && d.max_prediction == o.max_prediction && d.max_sample_gap == o.max_sample_gap && d.lift_hold == o.lift_hold &&
	         d.rejoin_min == o.rejoin_min && d.rejoin_slack == o.rejoin_slack && d.hold_travel == o.hold_travel && d.start_travel == o.start_travel && d.hold_speed == o.hold_speed &&
	         d.start_speed == o.start_speed && d.hold_recent == o.hold_recent && d.coast == o.coast && d.coast_after == o.coast_after && d.coast_max == o.coast_max &&
	         d.coast_speed == o.coast_speed && d.blend == o.blend && d.ease_min == o.ease_min && d.gap_blend == o.gap_blend && d.gap_blend_min == o.gap_blend_min && d.predict_travel == o.predict_travel); }
	int64_t t = 1000 * MS; clock_t0 = t; double x, y;

	reset(); x = 50; y = 240; down(t, 0, 1, 50, 240); drain(t);
	t = drag(t, 0, &x, &y, 500, 0, 10, 17 * MS);
	run_until(t + 17 * MS); up(t + 17 * MS, 0); drain(t + 17 * MS);
	t += 74 * MS; run_until(t); x += 500 * 0.074; down(t, 0, 2, (int)x, (int)y); drain(t);
	t = drag(t, 0, &x, &y, 500, 0, 10, 17 * MS); run_until(t + 30 * MS);
	printf("bridged drag: downs %d ups %d maxstep %.1f maxstill %.0f ms\n", downs, ups, maxstep, ms(maxstill));
	assert(downs == 1 && ups == 0 && maxstep <= 12 && maxstill <= 20 * MS);
	t += 30 * MS; up(t, 0); run_until(t + 120 * MS); assert(ups == 1);

	reset(); t += 500 * MS; x = 200; y = 240; down(t, 0, 3, 200, 240); drain(t);
	t = drag(t, 0, &x, &y, 0, 600, 3, 17 * MS);
	t = drag(t, 0, &x, &y, 0, -600, 3, 17 * MS);
	run_until(t + 17 * MS); up(t + 17 * MS, 0); drain(t);
	run_until(t + 40 * MS); assert(ups == 0);
	t += 60 * MS; down(t, 0, 4, (int)x, (int)(y - 600 * 0.06)); drain(t); run_until(t + 20 * MS);
	printf("near origin: downs %d ups %d\n", downs, ups);
	assert(downs == 1 && ups == 0);
	up(t + 20 * MS, 0); run_until(t + 200 * MS);

	reset(); t += 500 * MS; x = 40; y = 240; down(t, 0, 5, 40, 240); drain(t);
	t = drag(t, 0, &x, &y, 900, 0, 4, 17 * MS);
	run_until(t + 17 * MS); up(t + 17 * MS, 0); drain(t);
	t += 74 * MS; run_until(t); x += 900 * 0.074; down(t, 0, 6, (int)x, 240); drain(t); run_until(t + 20 * MS);
	printf("fast flick bridged: downs %d ups %d\n", downs, ups);
	assert(downs == 1 && ups == 0);
	up(t + 20 * MS, 0); run_until(t + 200 * MS);

	reset(); t += 500 * MS; x = 40; y = 240; down(t, 0, 7, 40, 240); drain(t);
	t = drag(t, 0, &x, &y, 1500, 0, 6, 17 * MS);
	int lx = (int)x; run_until(t + 17 * MS); up(t + 17 * MS, 0); drain(t);
	run_until(t + 60 * MS); int coasted = outx[0] - lx;
	run_until(t + 200 * MS);
	printf("real flick: coasted %d px, ups %d\n", coasted, ups);
	assert(ups == 1 && coasted > 5 && coasted <= 80);

	reset(); t += 500 * MS; x = 100; y = 100; down(t, 0, 8, 100, 100); drain(t);
	t = drag(t, 0, &x, &y, 100, 0, 12, 17 * MS);
	run_until(t + 17 * MS); up(t + 17 * MS, 0); drain(t);
	t += 60 * MS; run_until(t); down(t, 0, 9, (int)x, (int)y + 55); drain(t); run_until(t + 20 * MS);
	printf("second finger sideways: downs %d ups %d\n", downs, ups);
	assert(downs == 2 && ups == 1);
	up(t + 20 * MS, 0); run_until(t + 200 * MS);

	reset(); t += 500 * MS; x = 300; y = 100; down(t, 0, 10, 300, 100); drain(t);
	t = drag(t, 0, &x, &y, 800, 0, 6, 17 * MS);
	run_until(t + 17 * MS); up(t + 17 * MS, 0); drain(t);
	t += 50 * MS; run_until(t); down(t, 0, 11, (int)x - 45, (int)y); drain(t); run_until(t + 20 * MS);
	printf("touch behind stroke: downs %d ups %d\n", downs, ups);
	assert(downs == 2 && ups == 1);
	up(t + 20 * MS, 0); run_until(t + 200 * MS);

	reset(); t += 500 * MS; down(t, 0, 12, 240, 240); drain(t); run_until(t + 60 * MS); up(t + 60 * MS, 0); drain(t);
	printf("tap: ups %d\n", ups); assert(ups == 1);
	t += 100 * MS; run_until(t); down(t, 0, 13, 242, 240); up(t + 50 * MS, 0); drain(t);
	assert(downs == 2 && ups == 2);

	reset(); t += 500 * MS; x = 100; y = 300; down(t, 0, 14, 100, 300); drain(t);
	t = drag(t, 0, &x, &y, 400, 0, 8, 17 * MS);
	t += 150 * MS; run_until(t); up(t, 0); drain(t);
	printf("rest then lift: ups %d\n", ups); assert(ups == 1);

	reset(); t += 500 * MS; down(t, 0, 15, 100, 200); drain(t);
	for (int i = 1; i <= 8; i++) { t += 17 * MS; run_until(t); move(t, 0, 100 + i, 200); drain(t); }
	t += 17 * MS; run_until(t); up(t, 0); drain(t);
	printf("slow nudge: ups %d\n", ups); assert(ups == 1);

	reset(); t += 500 * MS; down(t, 0, 16, 240, 240); drain(t);
	for (int i = 1; i <= 3; i++) { t += 11 * MS; run_until(t); move(t, 0, 240 + 2 * i, 240); drain(t); }
	t += 5 * MS; run_until(t); up(t, 0); drain(t);
	t += 80 * MS; run_until(t); down(t, 0, 17, 244, 240); drain(t); t += 40 * MS; run_until(t); up(t, 0); drain(t);
	printf("jittery double tap: downs %d ups %d\n", downs, ups); assert(downs == 2 && ups == 2);

	reset(); t += 500 * MS; x = 40; y = 240; down(t, 0, 18, 40, 240); drain(t);
	t = drag(t, 0, &x, &y, 1500, 0, 6, 17 * MS);
	int stop_x = (int)x; t += 90 * MS; run_until(t); int shown = outx[0]; up(t, 0); drain(t);
	{ int prev = outx[0], back = 0;
	  for (int64_t u = t + 4 * MS; u <= t + 200 * MS; u += 4 * MS) { run_until(u); if (down_now[0] && prev - outx[0] > back) back = prev - outx[0]; prev = outx[0]; }
	  printf("lift after pause: shown %d at the lift, released at %d, last sample %d, ups %d, largest back step %d\n", shown, outx[0], stop_x, ups, back); assert(ups == 1 && abs(outx[0] - shown) <= 2 && back == 0); }

	reset(); t += 500 * MS; down(t, 0, 19, 100, 100); drain(t); t += 30 * MS; run_until(t);
	s(t, 0); ev(t, EV_ABS, ABS_MT_TRACKING_ID, -1); ev(t, EV_ABS, ABS_MT_TRACKING_ID, 20); ev(t, EV_ABS, ABS_MT_POSITION_X, 300); ev(t, EV_SYN, SYN_REPORT, 0); drain(t);
	printf("lift and land in one frame: downs %d ups %d\n", downs, ups); assert(downs == 2 && ups == 1);
	up(t + 20 * MS, 0); run_until(t + 200 * MS);

	reset(); t += 500 * MS; x = 40; y = 240; down(t, 0, 23, 40, 240); drain(t);
	t = drag(t, 0, &x, &y, 3000, 0, 4, 17 * MS);
	run_until(t + 17 * MS); up(t + 17 * MS, 0); drain(t);
	t += 74 * MS; run_until(t); down(t, 0, 24, (int)(x + 3000 * 0.074), 240); drain(t); run_until(t + 20 * MS);
	printf("very fast flick released when the glide is spent: downs %d ups %d\n", downs, ups);
	assert(downs == 2 && ups == 1);
	up(t + 20 * MS, 0); run_until(t + 200 * MS);

	reset(); t += 500 * MS; clock_t0 = t - 7 * MS; down(t, 0, 21, 100, 240); drain(t);
	{ int prev = 100, back = 0; move(t + 13 * MS, 0, 120, 240); drain(t + 13 * MS);
	  for (int i = 0; i < 8; i++) { run_until(t + 13 * MS + (i + 1) * 4 * MS); if (outx[0] < prev) back = 1; prev = outx[0]; if (i == 3) { move(t + 39 * MS, 0, 140, 240); drain(t + 39 * MS); } }
	  printf("early second sample: back step %d\n", back); assert(!back); }
	t += 100 * MS; up(t, 0); run_until(t + 200 * MS);

	reset(); t += 500 * MS; down(t, 0, 22, 200, 200); drain(t);
	s(t + 10 * MS, MAX_SLOTS + 3); ev(t + 10 * MS, EV_ABS, ABS_MT_TRACKING_ID, -1); ev(t + 10 * MS, EV_SYN, SYN_REPORT, 0); drain(t + 10 * MS);
	run_until(t + 200 * MS);
	printf("out of range slot: ups %d\n", ups); assert(ups == 0 && contacts[0].active);
	slot = 0; up(t + 210 * MS, 0); run_until(t + 400 * MS); assert(ups == 1);

	{ int worst = 0, cases = 0;
	  for (int slide = 2; slide <= 12; slide++) for (int ph = 0; ph < 8; ph++) for (int steps = 2; steps <= 4; steps++) {
	      reset(); t += 500 * MS + ph * MS; down(t, 0, 40, 200, 200); drain(t);
	      for (int i = 1; i <= steps; i++) { int64_t s2 = t + i * 25 * MS; while (clock_t0 + o.period <= s2) { run_until(clock_t0 + o.period); if (down_now[0]) { int d = (int)ceil(hypot(outx[0] - 200, outy[0] - 200)); if (d > worst) worst = d; } }
	          move(s2, 0, 200 + slide * i / steps, 200); drain(s2); }
	      int64_t u = t + steps * 25 * MS + 25 * MS;
	      while (clock_t0 + o.period <= u) { run_until(clock_t0 + o.period); if (down_now[0]) { int d = (int)ceil(hypot(outx[0] - 200, outy[0] - 200)); if (d > worst) worst = d; } }
	      up(u, 0); drain(u); run_until(u + 400 * MS); t = u + 400 * MS; cases++; }
	  printf("sloppy taps sliding 2-12 px (%d cases): output at most %d px from the down point\n", cases, worst); assert(worst <= 12); }

	{ struct input_event e[256]; ssize_t n; int toggles = 0, frames_with_both = 0, ax = -1;
	  reset(); t += 500 * MS; down(t, 1, 31, 300, 300); t += 20 * MS; run_until(t); down(t, 0, 30, 100, 100); t += 20 * MS; run_until(t);
	  while (read(pipefd[0], e, sizeof e) > 0);
	  s(t, 0); ev(t, EV_ABS, ABS_MT_POSITION_X, 110); ev(t, EV_SYN, SYN_REPORT, 0); s(t, 1); ev(t, EV_ABS, ABS_MT_POSITION_X, 310); ev(t, EV_SYN, SYN_REPORT, 0); run_until(t + 25 * MS);
	  ax = abs_x;
	  printf("pointer follows the oldest contact: ABS_X %d\n", ax); assert(ax >= 300 && ax <= 320);
	  run_until(t + 30 * MS);
	  { double px = 110, py = 100; t = drag(t, 0, &px, &py, 500, 0, 6, 17 * MS); }
	  up(t, 0);
	  printf("second finger lift during a two-finger touch: releasing %d\n", contacts[0].releasing); assert(!contacts[0].releasing);
	  t += 50 * MS; up(t, 1); run_until(t + 300 * MS); t += 300 * MS; down(t, 0, 34, 50, 50); t += 100 * MS; run_until(t); while (read(pipefd[0], e, sizeof e) > 0);
	  s(t, 0); ev(t, EV_ABS, ABS_MT_TRACKING_ID, -1); s(t, 1); ev(t, EV_ABS, ABS_MT_TRACKING_ID, 32); ev(t, EV_ABS, ABS_MT_POSITION_X, 200); ev(t, EV_ABS, ABS_MT_POSITION_Y, 200); ev(t, EV_SYN, SYN_REPORT, 0);
	  int btn_down = 0, btn_up = 0, seq = 0, lift_at = 0, land_at = 0;
	  while ((n = read(pipefd[0], e, sizeof e)) > 0) for (size_t i = 0; i < (size_t)n / sizeof e[0]; i++) {
	      if (e[i].type == EV_ABS && e[i].code == ABS_MT_TRACKING_ID) { if (e[i].value < 0) lift_at = ++seq; else if (e[i].value == 32) land_at = ++seq; }
	      if (e[i].type == EV_KEY && e[i].code == BTN_TOUCH) { toggles++; if (e[i].value) btn_down = 1; else btn_up = 1; }
	      if (e[i].type == EV_SYN) { if (btn_down && btn_up) frames_with_both++; btn_down = btn_up = 0; } }
	  printf("one contact ends and another starts in one frame: lift %d before landing %d, BTN_TOUCH events %d, frames with both %d\n", lift_at, land_at, toggles, frames_with_both);
	  assert(lift_at && land_at && lift_at < land_at && toggles == 2 && frames_with_both == 0);
	  run_until(t + 300 * MS); up(t + 300 * MS, 1); run_until(t + 600 * MS);
	  t += 700 * MS; down(t, 0, 33, 100, 100); run_until(t + 300 * MS);
	  assert(timer_armed == 0);
	  ev(t + 300 * MS, EV_ABS, ABS_MT_TOUCH_MAJOR, 9); ev(t + 300 * MS, EV_SYN, SYN_REPORT, 0);
	  printf("pressure-only frame on a resting finger: timer armed %d\n", timer_armed); assert(timer_armed == 0);
	  t += 300 * MS; move(t, 0, 140, 100); drain(t);
	  printf("first move after a rest is sent at once: x %d\n", outx[0]); assert(outx[0] == 140);
	  t += 200 * MS; run_until(t); move(t, 0, 143, 100); drain(t);
	  printf("small first move after a 200 ms rest is sent at once: x %d\n", outx[0]); assert(outx[0] == 143);
	  { int prev = outx[0], back = 0; t += 152 * MS; run_until(t); move(t, 0, 160, 100); drain(t); move(t + 14 * MS, 0, 175, 100); drain(t + 14 * MS);
	    for (int i = 1; i <= 6; i++) { run_until(t + i * 5 * MS); if (outx[0] < prev) back = 1; prev = outx[0]; }
	    printf("second sample soon after a long gap: back step %d\n", back); assert(!back); t += 30 * MS; }
	  up(t + 10 * MS, 0); run_until(t + 300 * MS); t += 300 * MS;
	  reset(); down(t, 0, 35, 200, 240); down(t, 1, 36, 280, 240); drain(t);
	  { double ax0 = 200, ay0 = 240, bx = 280, by = 240;
	    for (int i = 0; i < 6; i++) { t += 17 * MS; run_until(t); ax0 -= 5; bx += 5; move(t, 0, (int)ax0, (int)ay0); move(t, 1, (int)bx, (int)by); drain(t); } }
	  t += 60 * MS; run_until(t); up(t, 0); up(t, 1); drain(t);
	  printf("pinch lift: ups %d, held %d %d\n", ups, contacts[0].releasing, contacts[1].releasing); assert(ups == 2);
	  run_until(t + 200 * MS); t += 200 * MS; 
	{ reset(); x = 300; y = 240; down(t, 0, 37, 300, 240); drain(t);
	  t = drag(t, 0, &x, &y, -500, 0, 6, 17 * MS); run_until(t + 17 * MS); up(t + 17 * MS, 0); drain(t + 17 * MS);
	  int held = contacts[0].releasing; t += 32 * MS; run_until(t); down(t, 1, 38, 400, 240); drain(t);
	  printf("second finger in another slot during a held lift: held %d, then slot 0 down %d, ups %d\n", held, down_now[0], ups); assert(held && !down_now[0] && ups == 1);
	  t += 20 * MS; run_until(t); down(t, 0, 39, (int)x - 20, 240); drain(t);
	  printf("finger landing next to it: multi %d %d\n", contacts[0].multi, contacts[1].multi); assert(contacts[0].multi && contacts[1].multi);
	  up(t + 20 * MS, 0); up(t + 20 * MS, 1); run_until(t + 300 * MS); t += 300 * MS; }
	{ reset(); down(t, 1, 40, 300, 240); t += 50 * MS; run_until(t);
	  s(t, 1); ev(t, EV_ABS, ABS_MT_TRACKING_ID, -1); s(t, 0); ev(t, EV_ABS, ABS_MT_TRACKING_ID, 41); ev(t, EV_ABS, ABS_MT_POSITION_X, 100); ev(t, EV_ABS, ABS_MT_POSITION_Y, 100); ev(t, EV_SYN, SYN_REPORT, 0); drain(t);
	  printf("lift and land in other slots in one frame: multi %d\n", contacts[0].multi); assert(!contacts[0].multi);
	  up(t + 20 * MS, 0); run_until(t + 300 * MS); t += 300 * MS; }
	{ reset(); x = 300; y = 100; down(t, 1, 42, 300, 100); drain(t);
	  t = drag(t, 1, &x, &y, -500, 0, 6, 17 * MS); t += 17 * MS; run_until(t);
	  s(t, 1); ev(t, EV_ABS, ABS_MT_TRACKING_ID, -1); s(t, 0); ev(t, EV_ABS, ABS_MT_TRACKING_ID, 43); ev(t, EV_ABS, ABS_MT_POSITION_X, 400); ev(t, EV_ABS, ABS_MT_POSITION_Y, 100); ev(t, EV_SYN, SYN_REPORT, 0); drain(t);
	  printf("moving lift and a new finger in a lower slot in one frame: held %d, ups %d\n", contacts[1].releasing, ups); assert(!contacts[1].releasing && ups == 1);
	  up(t + 20 * MS, 0); run_until(t + 300 * MS); t += 300 * MS; }
	}

	reset(); t += 500 * MS; x = 40; y = 240; down(t, 0, 44, 40, 240); drain(t);
	t = drag(t, 0, &x, &y, 500, 0, 6, 25 * MS);
	for (int g = 70; g <= 200; g += 130) { double before = maxstep; maxstep = 0; t += g * MS; run_until(t); x += 100; move(t, 0, (int)x, 240); drain(t);
	  t = drag(t, 0, &x, &y, 500, 0, 4, 25 * MS);
	  printf("jump after a %d ms gap the kernel bridged: largest step %.0f px (before the gap %.0f)\n", g, maxstep, before); assert(maxstep < 40); }
	up(t, 0); run_until(t + 300 * MS); t += 300 * MS;

	reset(); o.ease_min = 250 * MS; x = 40; y = 240; down(t, 0, 45, 40, 240); drain(t);
	t = drag(t, 0, &x, &y, 500, 0, 6, 17 * MS); run_until(t + 165 * MS);
	printf("PREDICT_EASE_MS 250, 165 ms after the last sample: output %d, sample %d\n", outx[0], (int)x); assert(outx[0] > (int)x + 5);
	o.ease_min = 100 * MS; up(t + 170 * MS, 0); run_until(t + 450 * MS); t += 450 * MS;

	{ int in[2]; if (pipe(in) < 0) return 1; fcntl(in[0], F_SETFL, O_NONBLOCK); input_fd = in[0];
	  for (int i = 0; i < MAX_SLOTS; i++) kernel_id[i] = -1;
	  reset(); down(t, 1, 47, 300, 300); drain(t); t += 20 * MS; run_until(t);
	  kernel_id[0] = 48; kernel_x[0] = 60; kernel_y[0] = 420; move_after = 3; moved_x = 200; moved_y = 210; slot_reads = 0;
	  resync(t, &slot); flush(); drain(t); run_until(t + 20 * MS); t += 20 * MS;
	  printf("resync: slot 0 down %d at %d,%d, slot 1 down %d, snapshots %d\n", down_now[0], outx[0], outy[0], down_now[1], slot_reads);
	  assert(down_now[0] && outx[0] == 200 && outy[0] == 210 && !down_now[1]);
	  kernel_id[0] = -1; move_after = -1; resync(t, &slot); flush(); drain(t); run_until(t + 300 * MS); t += 300 * MS; assert(!down_now[0]);
	  close(in[0]); close(in[1]); input_fd = -1; }

	{ struct options saved = o; int bad = 0, n = 0;
	  o.period = NSEC_PER_SEC / 60; period = o.period; o.blend = 0.9; o.max_sample_gap = 100 * MS;
	  for (int g = 40; g <= 140; g += 5) for (int j = 31; j <= 120; j += 7, n++) {
	    reset(); x = 100; y = 240; down(t, 0, 49, 100, 240); drain(t);
	    t = drag(t, 0, &x, &y, 400, 0, 5, 30 * MS); t += g * MS; run_until(t); x += j; move(t, 0, (int)x, 240); drain(t);
	    run_until(t + 1500 * MS); t += 1500 * MS; if (outx[0] != (int)x) bad++;
	    up(t, 0); run_until(t + 400 * MS); t += 400 * MS; }
	  printf("blend settles on the finger at 60 Hz, BLEND 0.9, MAX_GAP_MS 100: %d of %d off\n", bad, n); assert(!bad);
	  o = saved; period = o.period; }
	for (int after = 9; after <= 93; after += 4) {
	  reset(); x = 100; down(t, 0, 50, 100, 100); down(t, 1, 51, 100, 300); drain(t);
	  for (int i = 0; i < 6; i++) { t += 30 * MS; run_until(t); x += 15; move(t, 0, (int)x, 100); move(t, 1, (int)x, 300); drain(t); }
	  t += 70 * MS; run_until(t); x += 75; move(t, 0, (int)x, 100); move(t, 1, (int)x, 300); drain(t);
	  t += after * MS; run_until(t); int pre = outx[0]; up(t, 0); up(t, 1); drain(t);
	  printf("two fingers lifted %d ms into a gap blend: shown %d, released at %d %d, finger at %d\n", after, pre, outx[0], outx[1], (int)x);
	  assert(outx[0] >= pre && outx[1] == outx[0] && (pre >= (int)x || outx[0] == (int)x)); run_until(t + 300 * MS); t += 300 * MS; }

	for (int land = -10; land <= 10; land += 10) for (int after = 0; after <= 25; after += 5) {
	  reset(); x = 50; y = 240; down(t, 0, 52, 50, 240); drain(t);
	  t = drag(t, 0, &x, &y, 400, 0, 8, 25 * MS); int lx = (int)x;
	  run_until(t + 80 * MS); up(t + 80 * MS, 0); drain(t + 80 * MS);
	  t += 155 * MS; run_until(t); down(t, 0, 53, lx + land, 240); drain(t);
	  run_until(t + after * MS); int pre = outx[0]; up(t + after * MS, 0); drain(t + after * MS);
	  printf("lift %d ms after a rejoin %+d px from the last sample, behind the coast: shown %d, released at %d\n", after, land, pre, outx[0]); assert(outx[0] >= pre);
	  run_until(t + 400 * MS); t += 400 * MS; }

	{ int bad = 0;
	  for (int ph = 0; ph < 84; ph++) {
	    reset(); t += 500 * MS; t -= t % o.period; t += clock_t0 % o.period; down(t, 1, 54, 450, 20); drain(t);
	    int64_t end = t + 300 * MS + ph * 100000LL, ts[] = { -207800, -116100, -69300, -11100 }; int xs[] = { 68, 189, 250, 327 }, ys[] = { 141, 131, 126, 120 };
	    run_until(end + ts[0] * 1000); down(end + ts[0] * 1000, 0, 55, xs[0], ys[0]); drain(end + ts[0] * 1000);
	    for (int i = 1; i < 4; i++) { run_until(end + ts[i] * 1000); move(end + ts[i] * 1000, 0, xs[i], ys[i]); drain(end + ts[i] * 1000); }
	    run_until(end); up(end, 0); drain(end); if (outx[0] != 327) bad++;
	    up(end, 1); drain(end); run_until(end + 300 * MS); t = end + 300 * MS; }
	  printf("two fingers, lift right after a gap blend settled, all tick phases: %d of 84 short of the finger\n", bad); assert(!bad); }
	{ reset(); t += 500 * MS; int64_t a = t; down(a, 0, 56, 220, 240); drain(a);
	  run_until(a + 40 * MS); move(a + 40 * MS, 0, 160, 240); drain(a + 40 * MS);
	  run_until(a + 80 * MS); move(a + 80 * MS, 0, 98, 240); drain(a + 80 * MS);
	  run_until(a + 110 * MS); move(a + 110 * MS, 0, 133, 240); drain(a + 110 * MS);
	  run_until(a + 170 * MS); up(a + 170 * MS, 0); drain(a + 170 * MS); run_until(a + 500 * MS);
	  printf("finger turns back, then lifts: released at %d, last sample 133\n", outx[0]); assert(outx[0] >= 128); t = a + 500 * MS; }
	{ reset(); t += 500 * MS; int64_t a = t; down(a, 0, 57, 240, 310); drain(a);
	  run_until(a + 33 * MS); move(a + 33 * MS, 0, 246, 311); drain(a + 33 * MS);
	  run_until(a + 182 * MS); move(a + 182 * MS, 0, 466, 333); drain(a + 182 * MS);
	  run_until(a + 185 * MS); up(a + 185 * MS, 0); drain(a + 185 * MS); run_until(a + 600 * MS);
	  printf("held lift during a gap blend: released at %d, last sample 466\n", outx[0]); assert(outx[0] >= 466); t = a + 600 * MS; }

	{ reset(); t += 500 * MS; int xs[] = { 100, 120, 140, 155, 162, 160 }; down(t, 0, 58, 100, 240); drain(t);
	  for (int i = 1; i < 6; i++) { t += 30 * MS; run_until(t); move(t, 0, xs[i], 240); drain(t); }
	  t += 20 * MS; run_until(t); up(t, 0); drain(t); t += 40 * MS; run_until(t); down(t, 0, 59, 161, 240); drain(t);
	  printf("drag settling with 2 px jitter, then a false lift: downs %d ups %d\n", downs, ups); assert(downs == 1 && ups == 0);
	  up(t + 20 * MS, 0); run_until(t + 400 * MS); t += 400 * MS; }
	{ int worst = 0;
	  for (int ph = 0; ph < 33; ph++) {
	    reset(); t += 500 * MS + ph * MS / 3; int px = 100; down(t, 0, 60, 100, 240); drain(t);
	    t += 33 * MS; run_until(t); px -= 3; move(t, 0, px, 240); drain(t);
	    for (int i = 0; i < 12; i++) { t += 33 * MS; run_until(t); px += 4; move(t, 0, px, 240); drain(t); }
	    t += ph * MS; run_until(t); int pre = outx[0]; up(t, 0); drain(t); run_until(t + 300 * MS);
	    if (pre - outx[0] > worst) { worst = pre - outx[0]; }
	    t += 300 * MS; }
	  printf("slow drag after a 3 px roll back at touch-down: worst release back-step %d px\n", worst); assert(worst <= 1); }

	{ int bad = 0, n = 0;
	  for (int after = 0; after <= 8; after += 2, n++) {
	    reset(); t += 600 * MS; run_until(t); x = 20; y = 240; down(t, 1, 61, 400, 100); drain(t); down(t, 0, 62, 20, 240); drain(t);
	    t = drag(t, 0, &x, &y, 1500, 0, 3, 50 * MS); t += 50 * MS; run_until(t); move(t, 0, (int)x, 240); drain(t);
	    run_until(t + after * MS); up(t + after * MS, 0); drain(t + after * MS); if (abs(outx[0] - (int)x) > 3) bad++;
	    up(t + after * MS + 5 * MS, 1); run_until(t + 200 * MS); t += 200 * MS; }
	  printf("second finger resting, first stops and lifts within a tick: %d of %d released off the finger\n", bad, n); assert(!bad); }

	{ reset(); t += 500 * MS; int xs[] = { 100, 160, 220, 280, 340, 310 }; down(t, 0, 63, 100, 240); drain(t);
	  for (int i = 1; i < 6; i++) { t += 40 * MS; run_until(t); move(t, 0, xs[i], 240); drain(t); }
	  t += 15 * MS; run_until(t); up(t, 0); drain(t); t += 45 * MS; run_until(t); down(t, 0, 64, 252, 240); drain(t);
	  printf("false lift right after the finger turned back: downs %d ups %d\n", downs, ups); assert(downs == 1 && ups == 0);
	  up(t + 20 * MS, 0); run_until(t + 400 * MS); t += 400 * MS; }

	{ int worst = 0; int cases[][3] = { { 60, 20, 30 }, { 40, 15, 25 }, { 30, 12, 40 } };
	  for (int k = 0; k < 3; k++) {
	    reset(); t += 500 * MS; int px = 100; down(t, 0, 65, px, 240); drain(t);
	    for (int i = 0; i < 4; i++) { t += 40 * MS; run_until(t); px += cases[k][0]; move(t, 0, px, 240); drain(t); }
	    t += 40 * MS; run_until(t); px -= cases[k][1]; move(t, 0, px, 240); drain(t);
	    t += cases[k][2] * MS; run_until(t); up(t, 0); drain(t); run_until(t + 400 * MS); t += 400 * MS;
	    if (abs(outx[0] - px) > worst) worst = abs(outx[0] - px); }
	  printf("real lift right after the finger turned back: released at most %d px from the finger\n", worst); assert(worst <= 3); }

	{ reset(); t += 500 * MS; s(t, 0); ev(t, EV_ABS, ABS_MT_TRACKING_ID, 66); ev(t, EV_ABS, ABS_MT_POSITION_X, 100); ev(t, EV_ABS, ABS_MT_POSITION_Y, 100);
	  s(t, 1); ev(t, EV_ABS, ABS_MT_TRACKING_ID, 67); ev(t, EV_ABS, ABS_MT_POSITION_X, 300); ev(t, EV_ABS, ABS_MT_POSITION_Y, 300); ev(t, EV_SYN, SYN_REPORT, 0); drain(t);
	  for (int i = 1; i <= 4; i++) { t += 20 * MS; run_until(t); s(t, 0); ev(t, EV_ABS, ABS_MT_POSITION_X, 100 - 10 * i); s(t, 1); ev(t, EV_ABS, ABS_MT_POSITION_X, 300 + 10 * i); ev(t, EV_SYN, SYN_REPORT, 0); drain(t); }
	  t += 20 * MS; run_until(t); s(t, 0); ev(t, EV_ABS, ABS_MT_TRACKING_ID, -1); s(t, 1); ev(t, EV_ABS, ABS_MT_TRACKING_ID, -1); ev(t, EV_SYN, SYN_REPORT, 0); drain(t); run_until(t + 300 * MS); t += 300 * MS; }
	printf("frames that change a contact together with another slot: %d\n", mixed_frames); assert(mixed_frames == 0);

	setenv("WALLVIEW_TOUCH_X", "1e13", 1); assert(env_number("WALLVIEW_TOUCH_X", 5) == 5);
	setenv("WALLVIEW_TOUCH_X", "nan", 1); assert(env_number("WALLVIEW_TOUCH_X", 5) == 5);
	setenv("WALLVIEW_TOUCH_X", "abc", 1); assert(env_number("WALLVIEW_TOUCH_X", 5) == 5);
	setenv("WALLVIEW_TOUCH_X", "7\n", 1); assert(env_number("WALLVIEW_TOUCH_X", 5) == 7);

	puts("ok");
	return 0;
}

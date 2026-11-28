#define main wt_main
#include "wallview-touch.c"
#undef main
#include <assert.h>
#define MS NSEC_PER_MSEC
static int pipefd[2];
static struct options o;
static int slot;
static int downs, ups, cur_slot, outx[MAX_SLOTS], outy[MAX_SLOTS], down_now[MAX_SLOTS];
static double maxstep;
static int64_t maxstill, still_since;
static int lastx = -1, lasty = -1;
static void ev(int64_t t, int type, int code, int value) { struct input_event e = { .type = type, .code = code, .value = value }; e.input_event_sec = t / NSEC_PER_SEC; e.input_event_usec = (t % NSEC_PER_SEC) / 1000; handle_event(&e, &slot); }
static void s(int64_t t, int sl) { ev(t, EV_ABS, ABS_MT_SLOT, sl); }
static void down(int64_t t, int sl, int id, int x, int y) { s(t, sl); ev(t, EV_ABS, ABS_MT_TRACKING_ID, id); ev(t, EV_ABS, ABS_MT_POSITION_X, x); ev(t, EV_ABS, ABS_MT_POSITION_Y, y); ev(t, EV_SYN, SYN_REPORT, 0); }
static void move(int64_t t, int sl, int x, int y) { s(t, sl); ev(t, EV_ABS, ABS_MT_POSITION_X, x); ev(t, EV_ABS, ABS_MT_POSITION_Y, y); ev(t, EV_SYN, SYN_REPORT, 0); }
static void up(int64_t t, int sl) { s(t, sl); ev(t, EV_ABS, ABS_MT_TRACKING_ID, -1); ev(t, EV_SYN, SYN_REPORT, 0); }
static void drain(int64_t t) {
	struct input_event e[256]; ssize_t n;
	while ((n = read(pipefd[0], e, sizeof e)) > 0)
		for (size_t i = 0; i < (size_t)n / sizeof e[0]; i++) {
			if (e[i].type != EV_ABS) continue;
			if (e[i].code == ABS_MT_SLOT) cur_slot = e[i].value;
			else if (e[i].code == ABS_MT_TRACKING_ID) { if (e[i].value < 0) { ups++; down_now[cur_slot] = 0; } else { downs++; down_now[cur_slot] = 1; lastx = -1; } }
			else if (e[i].code == ABS_MT_POSITION_X) outx[cur_slot] = e[i].value;
			else if (e[i].code == ABS_MT_POSITION_Y) outy[cur_slot] = e[i].value;
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
static void run_until(int64_t t) { while (clock_t0 + o.period <= t) { clock_t0 += o.period; resample(&o, clock_t0); drain(clock_t0); } }
static void reset(void) { memset(contacts, 0, sizeof contacts); downs = ups = 0; maxstep = 0; maxstill = 0; still_since = 0; lastx = -1; memset(down_now, 0, sizeof down_now); }
static void defaults(void) {
	setenv("WALLVIEW_TOUCH_LIFT_MS", "", 1);
	o = (struct options){ .period = NSEC_PER_SEC / 120, .latency = env_ms("WALLVIEW_TOUCH_LATENCY_MS", 16), .max_prediction = 30 * MS, .max_sample_gap = 150 * MS,
		.lift_hold = env_ms("WALLVIEW_TOUCH_LIFT_MS", 80), .rejoin_min = 30, .rejoin_slack = 0.75, .hold_travel = 12, .start_travel = 8, .hold_speed = 60, .start_speed = 150,
		.hold_recent = 100 * MS, .coast = 250 * MS, .coast_after = 85 * MS, .coast_max = 80, .coast_speed = 150, .blend = 0.7, .ease_min = 100 * MS, .predict_travel = 6, .debug = getenv("DBG") != NULL };
}
static int64_t drag(int64_t t, int sl, double *x, double *y, double vx, double vy, int n, int64_t dt) {
	for (int i = 0; i < n; i++) { t += dt; run_until(t); *x += vx * dt / NSEC_PER_SEC; *y += vy * dt / NSEC_PER_SEC; move(t, sl, (int)*x, (int)*y); drain(t); }
	return t;
}
int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); if (pipe(pipefd) < 0) return 1; fcntl(pipefd[0], F_SETFL, O_NONBLOCK); output = pipefd[1];
	timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK); defaults(); period = o.period; opts = &o;
	assert(o.lift_hold == 80 * MS);
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
	int stop_x = (int)x; t += 90 * MS; run_until(t); up(t, 0); drain(t);
	run_until(t + 50 * MS); int after_pause = outx[0];
	run_until(t + 200 * MS);
	printf("lift after pause: at %d, last %d, ups %d\n", after_pause, stop_x, ups); assert(ups == 1 && after_pause - stop_x <= 2);

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

	setenv("WALLVIEW_TOUCH_X", "1e13", 1); assert(env_number("WALLVIEW_TOUCH_X", 5) == 5);
	setenv("WALLVIEW_TOUCH_X", "nan", 1); assert(env_number("WALLVIEW_TOUCH_X", 5) == 5);
	setenv("WALLVIEW_TOUCH_X", "abc", 1); assert(env_number("WALLVIEW_TOUCH_X", 5) == 5);
	setenv("WALLVIEW_TOUCH_X", "7\n", 1); assert(env_number("WALLVIEW_TOUCH_X", 5) == 7);

	puts("ok");
	return 0;
}

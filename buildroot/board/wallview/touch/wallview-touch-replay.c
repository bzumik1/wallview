#define main wallview_touch_main
#include "wallview-touch.c"
#undef main

struct raw {
    int64_t time;
    int touch;
    int x;
    int y;
    int kept;
};

static struct raw *raws;
static int nraw;
static int pipe_out[2];
static int tracking;
static int raw_down;
static int raw_x = -1, raw_y = -1;
static int out_slot;
static int out_down;
static int out_x, out_y;
static int prev_x = -1, prev_y = -1;
static double prev_dx, prev_dy;
static int have_prev_step;
static int downs, ups, stalls, jerks, ticks;
static double *errors;
static int nerrors;
static double max_step;
static int release_back_steps;
static double release_back_worst;
static double *release_delays;
static double *release_offsets;
static int nrelease;
static double *tap_delays;
static int ntap;
static int64_t last_raw_touch;
static int64_t raw_up_at;
static int stroke_reach;
static int stroke_x0, stroke_y0;

static void
event(int64_t t, int type, int code, int value)
{
    struct input_event e = { .type = type, .code = code, .value = value };
    static int slot;

    e.input_event_sec = t / NSEC_PER_SEC;
    e.input_event_usec = (t % NSEC_PER_SEC) / 1000;
    handle_event(&e, &slot);
}

static struct raw *points;
static int npoints;

static void
build_points(void)
{
    points = malloc(nraw * sizeof(points[0]));
    for (int i = 0; i < nraw; i++) {
        const struct raw *r = &raws[i];

        if (!r->touch) {
            points[npoints++] = *r;
            continue;
        }
        if (npoints && points[npoints - 1].touch && points[npoints - 1].x == r->x && points[npoints - 1].y == r->y)
            continue;
        points[npoints++] = *r;
    }
}

static int
last_move(int64_t t, double *x, double *y, double *dx, double *dy)
{
    int i = npoints - 1;

    while (i >= 0 && (points[i].time > t || !points[i].touch))
        i--;
    if (i < 1 || !points[i - 1].touch)
        return 0;
    *x = points[i].x;
    *y = points[i].y;
    *dx = points[i].x - points[i - 1].x;
    *dy = points[i].y - points[i - 1].y;
    return 1;
}

static int
raw_position(int64_t t, double *x, double *y)
{
    for (int i = 1; i < npoints; i++) {
        if (points[i].time < t)
            continue;
        const struct raw *a = &points[i - 1];
        const struct raw *b = &points[i];

        if (!a->touch || !b->touch || b->time - a->time > 150 * NSEC_PER_MSEC || a->time > t)
            return 0;
        double f = b->time == a->time ? 1 : (double)(t - a->time) / (double)(b->time - a->time);

        *x = a->x + (b->x - a->x) * f;
        *y = a->y + (b->y - a->y) * f;
        return 1;
    }
    return 0;
}

static void
drain(int64_t t, int tick)
{
    struct input_event e[256];
    ssize_t n;

    while ((n = read(pipe_out[0], e, sizeof(e))) > 0)
        for (size_t i = 0; i < (size_t)n / sizeof(e[0]); i++) {
            if (e[i].type != EV_ABS)
                continue;
            if (e[i].code == ABS_MT_SLOT)
                out_slot = e[i].value;
            if (out_slot != 0)
                continue;
            if (e[i].code == ABS_MT_TRACKING_ID) {
                if (e[i].value < 0) {
                    double lx, ly, mx, my;
                    int moved = last_move(t, &lx, &ly, &mx, &my);

                    if (moved)
                        release_offsets[nrelease] = sqrt((out_x - lx) * (out_x - lx) + (out_y - ly) * (out_y - ly));
                    else
                        release_offsets[nrelease] = 0;
                    if (prev_x >= 0 && have_prev_step) {
                        double sx = out_x - prev_x, sy = out_y - prev_y, step = sqrt(sx * sx + sy * sy);
                        int against_stroke = moved && sx * mx + sy * my < 0;

                        if ((sx * prev_dx + sy * prev_dy < 0 || against_stroke) && step > 3) {
                            release_back_steps++;
                            if (step > release_back_worst)
                                release_back_worst = step;
                        }
                    }
                    ups++;
                    out_down = 0;
                    release_delays[nrelease++] = ms(t - last_raw_touch);
                    if (stroke_reach < 10 && raw_up_at)
                        tap_delays[ntap++] = ms(t - last_raw_touch);
                } else {
                    downs++;
                    out_down = 1;
                    prev_x = -1;
                }
            } else if (e[i].code == ABS_MT_POSITION_X) {
                out_x = e[i].value;
            } else if (e[i].code == ABS_MT_POSITION_Y) {
                out_y = e[i].value;
            }
        }
    if (!tick || !out_down)
        return;
    ticks++;
    double rx, ry, px, py;

    if (raw_position(t, &rx, &ry)) {
        errors[nerrors++] = sqrt((rx - out_x) * (rx - out_x) + (ry - out_y) * (ry - out_y));
        if (prev_x >= 0 && raw_position(t - period, &px, &py) &&
            sqrt((rx - px) * (rx - px) + (ry - py) * (ry - py)) > 2 && out_x == prev_x && out_y == prev_y)
            stalls++;
    }
    if (prev_x >= 0) {
        double step = sqrt((double)(out_x - prev_x) * (out_x - prev_x) + (double)(out_y - prev_y) * (out_y - prev_y));
        double ax = (out_x - prev_x) - prev_dx, ay = (out_y - prev_y) - prev_dy;

        if (step > max_step)
            max_step = step;
        if (have_prev_step && sqrt(ax * ax + ay * ay) > 6)
            jerks++;
        prev_dx = out_x - prev_x;
        prev_dy = out_y - prev_y;
        have_prev_step = 1;
    } else {
        have_prev_step = 0;
    }
    prev_x = out_x;
    prev_y = out_y;
}

static void
feed(const struct raw *r)
{
    if (r->touch && !raw_down) {
        raw_down = 1;
        raw_up_at = 0;
        stroke_reach = 0;
        stroke_x0 = r->x;
        stroke_y0 = r->y;
        event(r->time, EV_ABS, ABS_MT_SLOT, 0);
        event(r->time, EV_ABS, ABS_MT_TRACKING_ID, ++tracking);
        event(r->time, EV_ABS, ABS_MT_POSITION_X, r->x);
        event(r->time, EV_ABS, ABS_MT_POSITION_Y, r->y);
        event(r->time, EV_SYN, SYN_REPORT, 0);
        raw_x = r->x;
        raw_y = r->y;
    } else if (r->touch) {
        int changed = 0;

        if (r->x != raw_x) {
            event(r->time, EV_ABS, ABS_MT_POSITION_X, r->x);
            changed = 1;
        }
        if (r->y != raw_y) {
            event(r->time, EV_ABS, ABS_MT_POSITION_Y, r->y);
            changed = 1;
        }
        if (changed)
            event(r->time, EV_SYN, SYN_REPORT, 0);
        raw_x = r->x;
        raw_y = r->y;
    } else if (raw_down) {
        raw_down = 0;
        raw_up_at = r->time;
        event(r->time, EV_ABS, ABS_MT_TRACKING_ID, -1);
        event(r->time, EV_SYN, SYN_REPORT, 0);
    }
    if (r->touch && !r->kept) {
        int d = (int)sqrt((double)(r->x - stroke_x0) * (r->x - stroke_x0) + (double)(r->y - stroke_y0) * (r->y - stroke_y0));

        if (d > stroke_reach)
            stroke_reach = d;
        last_raw_touch = r->time;
    }
}

static int
cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;

    return x < y ? -1 : x > y;
}

static double
pct(double *v, int n, double p)
{
    if (!n)
        return 0;
    qsort(v, n, sizeof(v[0]), cmp);
    int i = (int)(n * p);

    return v[i >= n ? n - 1 : i];
}

int
main(int argc, char **argv)
{
    static struct options options;
    FILE *f = argc > 1 ? fopen(argv[1], "r") : NULL;
    char line[256];
    int cap = 1 << 16;
    const char *keep_env = getenv("REPLAY_KEEP_UNPRESSED");
    int hold_status = keep_env && atoi(keep_env) > 0;
    const char *timeout_env = getenv("REPLAY_KEEP_TIMEOUT_MS");
    int64_t keep_timeout = (timeout_env && *timeout_env ? atoll(timeout_env) : 110) * NSEC_PER_MSEC;
    int kept = 0;
    int64_t kept_at = 0;

    if (!f) {
        fprintf(stderr, "usage: wallview-touch-replay <touchprobe capture>\n");
        return 2;
    }
    raws = malloc(cap * sizeof(raws[0]));
    while (fgets(line, sizeof(line), f)) {
        unsigned long long start, end;
        unsigned count, id, x, y, extra;


        if (sscanf(line, "D %llu %llu %u %u %u %u %u", &start, &end, &count, &id, &x, &y, &extra) != 7)
            continue;
        if (nraw == cap)
            raws = realloc(raws, (cap *= 2) * sizeof(raws[0]));
        if (kept && (int64_t)start > kept_at + keep_timeout) {
            raws[nraw++] = (struct raw){ kept_at + keep_timeout, 0, 0, 0, 0 };
            kept = 0;
            if (nraw == cap)
                raws = realloc(raws, (cap *= 2) * sizeof(raws[0]));
        }
        if (count > 0 && id != 3 && hold_status && nraw && raws[nraw - 1].touch && kept < 2) {
            raws[nraw] = raws[nraw - 1];
            raws[nraw].kept = 1;
            raws[nraw++].time = (int64_t)start;
            if (!kept++)
                kept_at = (int64_t)start;
            continue;
        }
        kept = 0;
        raws[nraw++] = (struct raw){ (int64_t)start, count > 0 && id == 3, (int)y, (int)x, 0 };
    }
    fclose(f);
    if (!nraw) {
        fprintf(stderr, "wallview-touch-replay: no D lines\n");
        return 1;
    }
    build_points();
    load_options(&options);
    options.vblank = 0;
    opts = &options;
    period = options.period;
    if (pipe(pipe_out) < 0)
        return 1;
    fcntl(pipe_out[0], F_SETFL, O_NONBLOCK);
    fcntl(pipe_out[1], F_SETFL, O_NONBLOCK);
    output = pipe_out[1];
    timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    errors = malloc((size_t)(raws[nraw - 1].time - raws[0].time) / (size_t)period * sizeof(double) + 1024);
    release_delays = malloc(nraw * sizeof(double));
    release_offsets = malloc(nraw * sizeof(double));
    tap_delays = malloc(nraw * sizeof(double));

    int raw_strokes = 0, raw_short_gaps = 0;
    for (int i = 1; i < nraw; i++) {
        if (raws[i].touch && !raws[i - 1].touch) {
            raw_strokes++;
            for (int j = i - 1; j >= 0 && !raws[j].touch; j--)
                if (j == 0 || raws[j - 1].touch) {
                    if (j > 0 && raws[i].time - raws[j].time < 200 * NSEC_PER_MSEC)
                        raw_short_gaps++;
                    break;
                }
        }
    }

    int64_t tick = raws[0].time;
    for (int i = 0; i < nraw; i++) {
        while (tick + period <= raws[i].time) {
            tick += period;
            if (timer_armed)
                resample(&options, tick);
            drain(tick, timer_armed);
        }
        feed(&raws[i]);
        drain(raws[i].time, 0);
    }
    for (int k = 0; k < 60; k++) {
        tick += period;
        if (timer_armed)
            resample(&options, tick);
        drain(tick, timer_armed);
    }

    printf("raw: %d reads, %d touch-downs, %d downs within 200 ms of an up\n", nraw, raw_strokes + (raws[0].touch ? 1 : 0), raw_short_gaps);
    printf("out: %d downs, %d ups, %d ticks while down\n", downs, ups, ticks);
    printf("path error vs raw change points: p50 %.1f p95 %.1f max %.1f px\n", pct(errors, nerrors, .5), pct(errors, nerrors, .95), pct(errors, nerrors, 1));
    printf("steps: max %.1f px, jerks (acceleration > 6 px/tick^2) %d, stalls (finger moved, output did not) %d\n", max_step, jerks, stalls);
    printf("release frames stepping back more than 3 px: %d, worst %.1f px\n", release_back_steps, release_back_worst);
    printf("release distance from the last raw position: p50 %.1f p95 %.1f max %.1f px\n", pct(release_offsets, nrelease, .5), pct(release_offsets, nrelease, .95), pct(release_offsets, nrelease, 1));
    printf("release after last touch sample: p50 %.0f p95 %.0f max %.0f ms\n", pct(release_delays, nrelease, .5), pct(release_delays, nrelease, .95), pct(release_delays, nrelease, 1));
    printf("tap release after last pressed read: n %d p50 %.0f max %.0f ms\n", ntap, pct(tap_delays, ntap, .5), pct(tap_delays, ntap, 1));
    return 0;
}

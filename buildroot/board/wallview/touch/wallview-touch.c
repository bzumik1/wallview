#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <math.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define MAX_SLOTS 10
#define HISTORY 4
#define NSEC_PER_MSEC 1000000LL
#define NSEC_PER_SEC 1000000000LL

struct sample {
    int64_t time;
    int x;
    int y;
    int major;
};

struct contact {
    int tracking_id;
    int major;
    int x;
    int y;
    int dirty;
    int started;
    int ended;
    int active;
    struct sample history[HISTORY];
    int samples;
    int emitted_x;
    int emitted_y;
    int predicted;
    int fresh;
    int releasing;
    int64_t release_at;
    int64_t lift_at;
    int origin_x;
    int origin_y;
    int moved;
    int reach;
    double vx;
    double vy;
    int blend;
    double offset_x;
    double offset_y;
};

struct options {
    int64_t period;
    int64_t latency;
    int64_t max_prediction;
    int64_t max_sample_gap;
    int64_t lift_hold;
    int rejoin_min;
    double rejoin_slack;
    int hold_travel;
    int start_travel;
    double hold_speed;
    double hold_max_speed;
    double start_speed;
    int64_t hold_recent;
    int64_t coast;
    int64_t coast_after;
    int coast_max;
    double coast_speed;
    double blend;
    int debug;
};

static struct contact contacts[MAX_SLOTS];
static int min_x, max_x = 479, min_y, max_y = 479;
static int output;
static int timer;
static int timer_armed;
static int64_t period;
static const struct options *opts;
static struct input_event pending[256];
static int pending_count;

static int64_t
now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

static void
queue(int type, int code, int value)
{
    if (pending_count == (int)(sizeof(pending) / sizeof(pending[0])))
        return;
    pending[pending_count++] = (struct input_event){ .type = type, .code = code, .value = value };
}

static void
flush(void)
{
    if (!pending_count)
        return;
    queue(EV_SYN, SYN_REPORT, 0);
    if (write(output, pending, pending_count * sizeof(pending[0])) < 0)
        perror("wallview-touch: write");
    pending_count = 0;
}

static int
first_active(void)
{
    for (int slot = 0; slot < MAX_SLOTS; slot++)
        if (contacts[slot].active)
            return slot;
    return -1;
}

static void
emit_position(int slot, int x, int y)
{
    struct contact *c = &contacts[slot];

    if (x == c->emitted_x && y == c->emitted_y)
        return;
    queue(EV_ABS, ABS_MT_SLOT, slot);
    queue(EV_ABS, ABS_MT_POSITION_X, x);
    queue(EV_ABS, ABS_MT_POSITION_Y, y);
    if (first_active() == slot) {
        queue(EV_ABS, ABS_X, x);
        queue(EV_ABS, ABS_Y, y);
    }
    c->emitted_x = x;
    c->emitted_y = y;
}

static void
arm_timer(int on)
{
    struct itimerspec spec = { 0 };

    if (on == timer_armed)
        return;
    if (on)
        spec.it_interval.tv_nsec = spec.it_value.tv_nsec = period;
    if (timerfd_settime(timer, 0, &spec, NULL) < 0) {
        perror("wallview-touch: timerfd_settime");
        return;
    }
    timer_armed = on;
}

static void
add_sample(struct contact *c, int64_t time)
{
    if (c->samples == HISTORY) {
        memmove(c->history, c->history + 1, (HISTORY - 1) * sizeof(c->history[0]));
        c->samples--;
    }
    c->history[c->samples++] = (struct sample){ time, c->x, c->y, c->major };
    double dx = c->x - c->origin_x;
    double dy = c->y - c->origin_y;

    double reach = sqrt(dx * dx + dy * dy);

    if (reach > c->reach)
        c->reach = (int)reach;
    if (reach >= opts->hold_travel)
        c->moved = 1;
}

static double
velocity(const struct contact *c, double *vx, double *vy)
{
    int n = 1;

    *vx = *vy = 0;
    while (n < 3 && n < c->samples) {
        const struct sample *a = &c->history[c->samples - 1 - n];
        const struct sample *b = &c->history[c->samples - n];

        if (b->time <= a->time || b->time - a->time > opts->max_sample_gap)
            break;
        n++;
    }
    if (n < 2)
        return 0;

    const struct sample *s = &c->history[c->samples - n];
    double mt = 0, mx = 0, my = 0, stt = 0, stx = 0, sty = 0;

    for (int i = 0; i < n; i++) {
        mt += (double)(s[i].time - s[n - 1].time) / NSEC_PER_SEC;
        mx += s[i].x;
        my += s[i].y;
    }
    mt /= n;
    mx /= n;
    my /= n;
    for (int i = 0; i < n; i++) {
        double t = (double)(s[i].time - s[n - 1].time) / NSEC_PER_SEC - mt;

        stt += t * t;
        stx += t * (s[i].x - mx);
        sty += t * (s[i].y - my);
    }
    if (stt <= 0)
        return 0;
    *vx = stx / stt;
    *vy = sty / stt;
    return sqrt(*vx * *vx + *vy * *vy);
}

static int
round_half_away(double v)
{
    return v >= 0 ? (int)(v + 0.5) : -(int)(-v + 0.5);
}

static int
clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static double
ms(int64_t ns)
{
    return (double)ns / NSEC_PER_MSEC;
}

static void
release(int slot)
{
    struct contact *c = &contacts[slot];
    int first = first_active() == slot;
    struct sample *last = c->samples ? &c->history[c->samples - 1] : NULL;

    c->releasing = 0;
    if (last && (!c->predicted || c->fresh))
        emit_position(slot, clamp(last->x, min_x, max_x), clamp(last->y, min_y, max_y));
    c->fresh = 0;
    c->blend = 0;
    c->offset_x = c->offset_y = 0;
    queue(EV_ABS, ABS_MT_SLOT, slot);
    queue(EV_ABS, ABS_MT_TRACKING_ID, -1);
    c->active = 0;
    c->dirty = 0;
    int next = first_active();

    if (next < 0) {
        queue(EV_KEY, BTN_TOUCH, 0);
    } else if (first) {
        queue(EV_ABS, ABS_X, contacts[next].emitted_x);
        queue(EV_ABS, ABS_Y, contacts[next].emitted_y);
    }
}

static int
rejoins(int slot, int64_t time)
{
    const struct contact *c = &contacts[slot];

    if (!c->samples || time > c->release_at)
        return 0;
    const struct sample *last = &c->history[c->samples - 1];
    double dt = (double)(time - last->time) / NSEC_PER_SEC;
    double speed = sqrt(c->vx * c->vx + c->vy * c->vy);
    double dx = c->x - (last->x + c->vx * dt);
    double dy = c->y - (last->y + c->vy * dt);
    double limit = opts->rejoin_min + opts->rejoin_slack * speed * dt;
    double along = speed > 0 ? ((c->x - last->x) * c->vx + (c->y - last->y) * c->vy) / speed : 0;
    int ok = dx * dx + dy * dy <= limit * limit && along >= -opts->rejoin_min;

    if (opts->debug)
        fprintf(stderr, "wallview-touch: %s slot %d after %.0f ms, off %.0f px, limit %.0f px, along %.0f px\n",
                ok ? "rejoin" : "reject", slot, ms(time - c->lift_at), sqrt(dx * dx + dy * dy), limit, along);
    return ok;
}

static int
holds(int slot, int64_t time)
{
    struct contact *c = &contacts[slot];

    if (c->samples < 2)
        return 0;
    const struct sample *last = &c->history[c->samples - 1];
    double speed = velocity(c, &c->vx, &c->vy);
    int age_ok = time - last->time <= opts->hold_recent;
    int hold = age_ok && (opts->hold_max_speed <= 0 || speed <= opts->hold_max_speed) &&
               ((c->reach >= opts->start_travel && speed >= opts->start_speed) ||
                (c->moved && speed >= opts->hold_speed));

    if (opts->debug) {
        const struct sample *s = c->history;
        int n = c->samples;

        fprintf(stderr, "wallview-touch: lift slot %d at %.0f,%d after %.0f ms, %.0f px/s, moved %d, major %d %d %d: %s\n",
                slot, (double)last->x, last->y, ms(time - last->time), speed, c->moved,
                n >= 3 ? s[n - 3].major : -1, n >= 2 ? s[n - 2].major : -1, s[n - 1].major,
                hold ? "hold" : "release");
    }
    return hold;
}

static void
expire_releases(int64_t now)
{
    for (int slot = 0; slot < MAX_SLOTS; slot++)
        if (contacts[slot].releasing && now >= contacts[slot].release_at) {
            if (opts->debug)
                fprintf(stderr, "wallview-touch: expire slot %d after %.0f ms\n", slot,
                        ms(now - contacts[slot].lift_at));
            release(slot);
        }
}

static void
handle_frame(int64_t time)
{
    for (int slot = 0; slot < MAX_SLOTS; slot++) {
        struct contact *c = &contacts[slot];

        if (c->started && c->releasing && rejoins(slot, time)) {
            c->started = 0;
            c->releasing = 0;
            c->dirty = 0;
            c->fresh = 1;
            c->blend = 1;
            add_sample(c, time);
        } else if (c->started) {
            if (c->active) {
                release(slot);
                flush();
            }
            c->started = 0;
            c->active = 1;
            c->samples = 0;
            c->moved = 0;
            c->reach = 0;
            c->blend = 0;
            c->offset_x = c->offset_y = 0;
            c->origin_x = c->x;
            c->origin_y = c->y;
            add_sample(c, time);
            queue(EV_ABS, ABS_MT_SLOT, slot);
            queue(EV_ABS, ABS_MT_TRACKING_ID, c->tracking_id);
            queue(EV_ABS, ABS_MT_TOUCH_MAJOR, c->major);
            c->emitted_x = -1;
            c->emitted_y = -1;
            c->predicted = 0;
            c->fresh = 0;
            if (first_active() == slot)
                queue(EV_KEY, BTN_TOUCH, 1);
            emit_position(slot, c->x, c->y);
            c->dirty = 0;
        } else if (c->ended) {
            c->ended = 0;
            if (!c->active || c->releasing) {
                c->dirty = 0;
            } else if (opts->lift_hold > 0 && holds(slot, time)) {
                c->releasing = 1;
                c->lift_at = time;
                c->release_at = time + opts->lift_hold;
                c->dirty = 0;
            } else {
                release(slot);
            }
        } else if (c->dirty) {
            c->dirty = 0;
            c->fresh = 1;
            add_sample(c, time);
        }
    }
    flush();
    arm_timer(first_active() >= 0);
}

static int
interpolate(int a, int b, int64_t ta, int64_t tb, int64_t t)
{
    if (tb == ta)
        return b;
    double alpha = (double)(t - ta) / (double)(tb - ta);
    return round_half_away(a + (b - a) * alpha);
}

static void
coast(const struct contact *c, const struct sample *last, int64_t target, int *x, int *y)
{
    double speed = sqrt(c->vx * c->vx + c->vy * c->vy);

    if (opts->coast <= 0 || speed <= 0 || speed < opts->coast_speed || target <= last->time ||
        c->lift_at - last->time > opts->coast_after)
        return;
    double tau = (double)opts->coast / NSEC_PER_SEC;
    double t = (double)(target - last->time) / NSEC_PER_SEC;
    double distance = speed * tau * (1 - exp(-t / tau));

    if (distance > opts->coast_max)
        distance = opts->coast_max;
    *x = round_half_away(last->x + c->vx / speed * distance);
    *y = round_half_away(last->y + c->vy / speed * distance);
}

static void
predict(const struct contact *c, const struct sample *last, int64_t target, int *x, int *y)
{
    const struct sample *a = &c->history[c->samples - 2];
    int64_t interval = last->time - a->time;
    int64_t age = target - last->time;
    int64_t ahead = age;
    int64_t limit = interval < opts->max_prediction ? interval : opts->max_prediction;
    int64_t ease_from = interval + interval / 2;
    double vx, vy;

    if (interval > opts->max_sample_gap || age <= 0 || age > opts->max_sample_gap)
        return;
    if (ahead > limit)
        ahead = limit;
    if (age > ease_from) {
        double keep = 1 - (double)(age - ease_from) / (double)(4 * period);

        ahead = keep > 0 ? (int64_t)(ahead * keep) : 0;
    }
    velocity(c, &vx, &vy);
    *x = round_half_away(last->x + vx * ahead / NSEC_PER_SEC);
    *y = round_half_away(last->y + vy * ahead / NSEC_PER_SEC);
}

static void
resample(const struct options *options, int64_t now)
{
    int64_t target = now - options->latency;

    expire_releases(now);

    for (int slot = 0; slot < MAX_SLOTS; slot++) {
        struct contact *c = &contacts[slot];

        if (!c->active || !c->samples)
            continue;

        struct sample *last = &c->history[c->samples - 1];
        int x = last->x;
        int y = last->y;

        c->predicted = 0;
        if (target < last->time) {
            for (int i = c->samples - 1; i > 0; i--) {
                struct sample *a = &c->history[i - 1];
                struct sample *b = &c->history[i];

                if (a->time <= target && target <= b->time && b->time - a->time <= options->max_sample_gap) {
                    x = interpolate(a->x, b->x, a->time, b->time, target);
                    y = interpolate(a->y, b->y, a->time, b->time, target);
                    break;
                }
            }
        } else {
            if (c->releasing)
                coast(c, last, target, &x, &y);
            else if (c->samples >= 2)
                predict(c, last, target, &x, &y);
            c->predicted = x != last->x || y != last->y;
        }
        if (c->blend) {
            c->blend = 0;
            if (c->emitted_x >= 0) {
                c->offset_x = c->emitted_x - x;
                c->offset_y = c->emitted_y - y;
            }
        }
        if (c->offset_x != 0 || c->offset_y != 0) {
            x = round_half_away(x + c->offset_x);
            y = round_half_away(y + c->offset_y);
            c->offset_x *= opts->blend;
            c->offset_y *= opts->blend;
            if (c->offset_x > -0.5 && c->offset_x < 0.5 && c->offset_y > -0.5 && c->offset_y < 0.5)
                c->offset_x = c->offset_y = 0;
            c->predicted = 1;
        }
        c->fresh = 0;
        emit_position(slot, clamp(x, min_x, max_x), clamp(y, min_y, max_y));
    }
    flush();
    arm_timer(first_active() >= 0);
}

static int dropped;
static int input_fd;

static int
read_slots(int code, int32_t *values)
{
    struct {
        uint32_t code;
        int32_t values[MAX_SLOTS];
    } slots = { .code = code };

    if (ioctl(input_fd, EVIOCGMTSLOTS(sizeof(slots)), &slots) < 0)
        return -1;
    memcpy(values, slots.values, sizeof(slots.values));
    return 0;
}

static void
load_positions(void)
{
    int32_t xs[MAX_SLOTS], ys[MAX_SLOTS];

    if (read_slots(ABS_MT_POSITION_X, xs) < 0 || read_slots(ABS_MT_POSITION_Y, ys) < 0)
        return;
    for (int slot = 0; slot < MAX_SLOTS; slot++) {
        if (contacts[slot].active && !contacts[slot].releasing &&
            (contacts[slot].x != xs[slot] || contacts[slot].y != ys[slot]))
            contacts[slot].dirty = 1;
        contacts[slot].x = xs[slot];
        contacts[slot].y = ys[slot];
    }
}

static void
resync(int64_t time, int *slot)
{
    struct input_absinfo current_slot;
    int32_t ids[MAX_SLOTS];

    if (ioctl(input_fd, EVIOCGABS(ABS_MT_SLOT), &current_slot) == 0 && current_slot.value >= 0 &&
        current_slot.value < MAX_SLOTS)
        *slot = current_slot.value;
    if (read_slots(ABS_MT_TRACKING_ID, ids) < 0)
        return;
    load_positions();
    for (int slot = 0; slot < MAX_SLOTS; slot++) {
        struct contact *c = &contacts[slot];

        if (ids[slot] < 0) {
            if ((c->active || c->started) && !c->releasing) {
                c->ended = 1;
                c->started = 0;
            }
        } else if (!c->active || c->releasing || c->tracking_id != ids[slot]) {
            c->tracking_id = ids[slot];
            c->started = 1;
            c->ended = 0;
        }
    }
    handle_frame(time);
}

static void
handle_event(const struct input_event *event, int *slot)
{
    struct contact *c = &contacts[*slot];
    int64_t time = event->input_event_sec * NSEC_PER_SEC + event->input_event_usec * 1000LL;

    if (event->type == EV_SYN && event->code == SYN_DROPPED) {
        dropped = 1;
        return;
    }
    if (dropped) {
        if (event->type == EV_SYN && event->code == SYN_REPORT) {
            dropped = 0;
            resync(time, slot);
        }
        return;
    }
    if (event->type == EV_ABS) {
        switch (event->code) {
        case ABS_MT_SLOT:
            if (event->value >= 0 && event->value < MAX_SLOTS)
                *slot = event->value;
            break;
        case ABS_MT_TRACKING_ID:
            if (event->value < 0) {
                c->ended = c->active || c->started;
                c->started = 0;
            } else {
                c->tracking_id = event->value;
                c->started = 1;
                c->ended = 0;
            }
            break;
        case ABS_MT_POSITION_X:
            c->x = event->value;
            c->dirty = 1;
            break;
        case ABS_MT_POSITION_Y:
            c->y = event->value;
            c->dirty = 1;
            break;
        case ABS_MT_TOUCH_MAJOR:
            c->major = event->value;
            break;
        }
    } else if (event->type == EV_SYN && event->code == SYN_REPORT) {
        handle_frame(time);
    }
}

static int
create_output(int input)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    unsigned long abs_bits[(ABS_CNT + 63) / 64] = { 0 };

    if (fd < 0)
        return -1;
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
    ioctl(input, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
    for (int code = 0; code < ABS_CNT; code++) {
        if (!(abs_bits[code / 64] & (1ul << (code % 64))))
            continue;
        struct uinput_abs_setup setup = { .code = code };

        if (ioctl(input, EVIOCGABS(code), &setup.absinfo) < 0)
            continue;
        if (code == ABS_MT_SLOT && setup.absinfo.maximum >= MAX_SLOTS)
            setup.absinfo.maximum = MAX_SLOTS - 1;
        if (code == ABS_MT_POSITION_X) {
            min_x = setup.absinfo.minimum;
            max_x = setup.absinfo.maximum;
        } else if (code == ABS_MT_POSITION_Y) {
            min_y = setup.absinfo.minimum;
            max_y = setup.absinfo.maximum;
        }
        ioctl(fd, UI_SET_ABSBIT, code);
        ioctl(fd, UI_ABS_SETUP, &setup);
    }

    struct uinput_setup setup = { .id = { .bustype = BUS_VIRTUAL, .vendor = 0x1209, .product = 0x5754, .version = 1 } };

    strcpy(setup.name, "WallView touchscreen");
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int
open_touchscreen(const char *device)
{
    if (device)
        return open(device, O_RDONLY | O_NONBLOCK);
    for (int i = 0; i < 32; i++) {
        char path[32];
        char name[128] = "";
        int fd;

        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && !strncmp(name, "Hynitron", 8))
            return fd;
        close(fd);
    }
    errno = ENODEV;
    return -1;
}

static double
env_number(const char *name, double fallback)
{
    const char *value = getenv(name);
    char *end;
    double number;

    if (!value || !*value)
        return fallback;
    number = strtod(value, &end);
    while (*end == ' ' || *end == '\n' || *end == '\t')
        end++;
    if (*end || !isfinite(number) || number < 0) {
        fprintf(stderr, "wallview-touch: ignoring %s=%s\n", name, value);
        return fallback;
    }
    return number;
}

static int64_t
env_ms(const char *name, double fallback)
{
    return (int64_t)(env_number(name, fallback) * NSEC_PER_MSEC);
}

static void
load_options(struct options *options)
{
    int rate = (int)env_number("WALLVIEW_TOUCH_RATE", 120);

    *options = (struct options){
        .period = NSEC_PER_SEC / (rate >= 2 && rate <= 1000 ? rate : 120),
        .latency = env_ms("WALLVIEW_TOUCH_LATENCY_MS", 16),
        .max_prediction = env_ms("WALLVIEW_TOUCH_PREDICTION_MS", 30),
        .max_sample_gap = env_ms("WALLVIEW_TOUCH_MAX_GAP_MS", 150),
        .lift_hold = env_ms("WALLVIEW_TOUCH_LIFT_MS", 100),
        .rejoin_min = (int)env_number("WALLVIEW_TOUCH_REJOIN_PX", 30),
        .rejoin_slack = env_number("WALLVIEW_TOUCH_REJOIN_SLACK", 0.75),
        .hold_travel = (int)env_number("WALLVIEW_TOUCH_HOLD_TRAVEL_PX", 12),
        .start_travel = (int)env_number("WALLVIEW_TOUCH_START_TRAVEL_PX", 8),
        .hold_speed = env_number("WALLVIEW_TOUCH_HOLD_PX_PER_S", 60),
        .hold_max_speed = env_number("WALLVIEW_TOUCH_HOLD_MAX_PX_PER_S", 0),
        .start_speed = env_number("WALLVIEW_TOUCH_START_PX_PER_S", 150),
        .hold_recent = env_ms("WALLVIEW_TOUCH_HOLD_RECENT_MS", 100),
        .coast = env_ms("WALLVIEW_TOUCH_COAST_MS", 40),
        .coast_after = env_ms("WALLVIEW_TOUCH_COAST_AFTER_MS", 40),
        .coast_max = (int)env_number("WALLVIEW_TOUCH_COAST_PX", 40),
        .coast_speed = env_number("WALLVIEW_TOUCH_COAST_PX_PER_S", 150),
        .blend = env_number("WALLVIEW_TOUCH_BLEND", 0.7),
        .debug = (int)env_number("WALLVIEW_TOUCH_DEBUG", 0),
    };
    if (options->blend > 0.95)
        options->blend = 0.7;
}

int
main(int argc, char **argv)
{
    const char *device = argc > 1 ? argv[1] : NULL;
    struct options options;

    load_options(&options);
    int clock = CLOCK_MONOTONIC;
    int input = open_touchscreen(device);

    if (input < 0 || ioctl(input, EVIOCSCLOCKID, &clock) < 0) {
        fprintf(stderr, "wallview-touch: touchscreen: %s\n", strerror(errno));
        return 1;
    }
    input_fd = input;

    for (int waited = 0;; waited++) {
        unsigned long keys[(KEY_CNT + 63) / 64] = { 0 };

        if (ioctl(input, EVIOCGKEY(sizeof(keys)), keys) < 0 ||
            !(keys[BTN_TOUCH / 64] & (1ul << (BTN_TOUCH % 64))))
            break;
        if (waited == 100) {
            fprintf(stderr, "wallview-touch: touch held for 5 s, grabbing anyway\n");
            break;
        }
        usleep(50000);
    }
    if (ioctl(input, EVIOCGRAB, 1) < 0) {
        fprintf(stderr, "wallview-touch: grab: %s\n", strerror(errno));
        return 1;
    }
    {
        struct input_event stale[64];

        while (read(input, stale, sizeof(stale)) > 0)
            ;
    }
    load_positions();
    for (int i = 0; i < MAX_SLOTS; i++)
        contacts[i].dirty = 0;
    output = create_output(input);
    if (output < 0) {
        fprintf(stderr, "wallview-touch: uinput: %s\n", strerror(errno));
        return 1;
    }

    struct sched_param param = { .sched_priority = 40 };

    sched_setscheduler(0, SCHED_FIFO, &param);

    period = options.period;
    opts = &options;
    timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (timer < 0) {
        fprintf(stderr, "wallview-touch: timerfd: %s\n", strerror(errno));
        return 1;
    }

    int slot = 0;
    struct input_absinfo current_slot;

    if (ioctl(input, EVIOCGABS(ABS_MT_SLOT), &current_slot) == 0 && current_slot.value >= 0 &&
        current_slot.value < MAX_SLOTS)
        slot = current_slot.value;

    struct pollfd fds[2] = { { input, POLLIN, 0 }, { timer, POLLIN, 0 } };

    for (;;) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }
        if (fds[0].revents & (POLLERR | POLLHUP))
            return 1;
        if (fds[0].revents & POLLIN) {
            struct input_event events[64];
            ssize_t size;

            while ((size = read(input, events, sizeof(events))) > 0)
                for (size_t i = 0; i < (size_t)size / sizeof(events[0]); i++)
                    handle_event(&events[i], &slot);
        }
        if (fds[1].revents & POLLIN) {
            uint64_t expirations;

            if (read(timer, &expirations, sizeof(expirations)) > 0)
                resample(&options, now_ns());
        }
    }
}

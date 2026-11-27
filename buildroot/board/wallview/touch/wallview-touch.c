#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
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
};

struct options {
    int64_t period;
    int64_t latency;
    int64_t max_prediction;
    int64_t max_sample_gap;
};

static struct contact contacts[MAX_SLOTS];
static int output;
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
add_sample(struct contact *c, int64_t time)
{
    if (c->samples == HISTORY) {
        memmove(c->history, c->history + 1, (HISTORY - 1) * sizeof(c->history[0]));
        c->samples--;
    }
    c->history[c->samples++] = (struct sample){ time, c->x, c->y };
}

static void
handle_frame(int64_t time)
{
    for (int slot = 0; slot < MAX_SLOTS; slot++) {
        struct contact *c = &contacts[slot];

        if (c->started) {
            c->started = 0;
            c->active = 1;
            c->samples = 0;
            add_sample(c, time);
            queue(EV_ABS, ABS_MT_SLOT, slot);
            queue(EV_ABS, ABS_MT_TRACKING_ID, c->tracking_id);
            queue(EV_ABS, ABS_MT_TOUCH_MAJOR, c->major);
            c->emitted_x = -1;
            c->emitted_y = -1;
            if (first_active() == slot)
                queue(EV_KEY, BTN_TOUCH, 1);
            emit_position(slot, c->x, c->y);
            c->dirty = 0;
        } else if (c->ended) {
            c->ended = 0;
            emit_position(slot, c->x, c->y);
            queue(EV_ABS, ABS_MT_SLOT, slot);
            queue(EV_ABS, ABS_MT_TRACKING_ID, -1);
            c->active = 0;
            c->dirty = 0;
            if (first_active() < 0)
                queue(EV_KEY, BTN_TOUCH, 0);
        } else if (c->dirty) {
            c->dirty = 0;
            add_sample(c, time);
        }
    }
    flush();
}

static int
interpolate(int a, int b, int64_t ta, int64_t tb, int64_t t)
{
    if (tb == ta)
        return b;
    double alpha = (double)(t - ta) / (double)(tb - ta);
    return (int)(a + (b - a) * alpha + (b >= a ? 0.5 : -0.5));
}

static void
resample(const struct options *options, int64_t now)
{
    int64_t target = now - options->latency;

    for (int slot = 0; slot < MAX_SLOTS; slot++) {
        struct contact *c = &contacts[slot];

        if (!c->active || !c->samples)
            continue;

        struct sample *last = &c->history[c->samples - 1];
        int x = last->x;
        int y = last->y;

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
        } else if (c->samples >= 2) {
            struct sample *a = &c->history[c->samples - 2];
            int64_t ahead = target - last->time;

            if (ahead > options->max_prediction)
                ahead = options->max_prediction;
            if (last->time - a->time <= options->max_sample_gap && ahead > 0) {
                x = interpolate(a->x, last->x, a->time, last->time, last->time + ahead);
                y = interpolate(a->y, last->y, a->time, last->time, last->time + ahead);
            }
        }
        emit_position(slot, x, y);
    }
    flush();
}

static void
handle_event(const struct input_event *event, int *slot)
{
    struct contact *c = &contacts[*slot];

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
        handle_frame(event->input_event_sec * NSEC_PER_SEC + event->input_event_usec * 1000LL);
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

static int64_t
env_ms(const char *name, double fallback)
{
    const char *value = getenv(name);

    return (int64_t)((value ? atof(value) : fallback) * NSEC_PER_MSEC);
}

int
main(int argc, char **argv)
{
    const char *device = argc > 1 ? argv[1] : NULL;
    struct options options = {
        .period = NSEC_PER_SEC / (getenv("WALLVIEW_TOUCH_RATE") ? atoi(getenv("WALLVIEW_TOUCH_RATE")) : 120),
        .latency = env_ms("WALLVIEW_TOUCH_LATENCY_MS", 8),
        .max_prediction = env_ms("WALLVIEW_TOUCH_PREDICTION_MS", 12),
        .max_sample_gap = env_ms("WALLVIEW_TOUCH_MAX_GAP_MS", 60),
    };
    int clock = CLOCK_MONOTONIC;
    int input = open_touchscreen(device);

    if (input < 0 || ioctl(input, EVIOCSCLOCKID, &clock) < 0) {
        fprintf(stderr, "wallview-touch: touchscreen: %s\n", strerror(errno));
        return 1;
    }
    output = create_output(input);
    if (output < 0) {
        fprintf(stderr, "wallview-touch: uinput: %s\n", strerror(errno));
        return 1;
    }
    if (ioctl(input, EVIOCGRAB, 1) < 0) {
        fprintf(stderr, "wallview-touch: grab: %s\n", strerror(errno));
        return 1;
    }

    struct sched_param param = { .sched_priority = 40 };

    sched_setscheduler(0, SCHED_FIFO, &param);

    int timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    struct itimerspec spec = {
        .it_interval = { 0, options.period },
        .it_value = { 0, options.period },
    };

    timerfd_settime(timer, 0, &spec, NULL);

    int slot = 0;
    struct pollfd fds[2] = { { input, POLLIN, 0 }, { timer, POLLIN, 0 } };

    for (;;) {
        if (poll(fds, 2, -1) < 0 && errno != EINTR)
            return 1;
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

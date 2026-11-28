#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <drm/drm.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <math.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
    double dir_x;
    double dir_y;
    int turned;
    double offset_x;
    double offset_y;
    int coasting;
    double coast_x;
    double coast_y;
    int64_t coast_at;
    uint64_t order;
    int multi;
};

struct options {
    int64_t period;
    int64_t latency;
    int64_t max_prediction;
    int64_t max_sample_gap;
    int64_t lift_hold;
    int64_t tap_hold;
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
    int64_t ease_min;
    int64_t gap_blend;
    int64_t gap_blend_min;
    int predict_travel;
    int debug;
    int vblank;
    const char *drm_path;
    const char *drm_driver;
    int64_t vblank_offset;
};

static struct contact contacts[MAX_SLOTS];
static int min_x, max_x = 479, min_y, max_y = 479;
static int output = -1;
static int timer;
static int timer_armed;
static int64_t period;
static int drm = -1;
static int drm_failed;
static int drm_warned;
static int64_t drm_retry_at;
static int64_t drm_backoff = 5 * NSEC_PER_SEC;
static int vblank_pending;
static int phase_timer = -1;
static int64_t frame = NSEC_PER_SEC / 60;
static int64_t last_vblank;
static int64_t vblank_retry_at;
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

static int frame_slot = -1;
static int sent_slot = -1;
static void flush(void);

static void
queue(int type, int code, int value)
{
    if (type == EV_ABS && code == ABS_MT_SLOT) {
        if (pending_count && frame_slot >= 0 && value != frame_slot)
            flush();
        frame_slot = value;
    }
    if (pending_count == (int)(sizeof(pending) / sizeof(pending[0])))
        return;
    pending[pending_count++] = (struct input_event){ .type = type, .code = code, .value = value };
}

static int first_active(void);
static int touch_sent;
static uint64_t started_count;

static int sent_down[MAX_SLOTS];

static void
flush(void)
{
    int down = 0;

    for (int i = 0, slot = sent_slot; i < pending_count; i++) {
        if (pending[i].type != EV_ABS)
            continue;
        if (pending[i].code == ABS_MT_SLOT)
            slot = pending[i].value;
        else if (pending[i].code == ABS_MT_TRACKING_ID && slot >= 0 && slot < MAX_SLOTS)
            sent_down[slot] = pending[i].value >= 0;
        sent_slot = slot;
    }
    for (int slot = 0; slot < MAX_SLOTS; slot++)
        down |= sent_down[slot];

    if (down != touch_sent) {
        queue(EV_KEY, BTN_TOUCH, down);
        touch_sent = down;
    }
    if (!pending_count)
        return;
    queue(EV_SYN, SYN_REPORT, 0);
    if (write(output, pending, pending_count * sizeof(pending[0])) < 0)
        perror("wallview-touch: write");
    pending_count = 0;
    frame_slot = -1;
}

static int
first_active(void)
{
    for (int slot = 0; slot < MAX_SLOTS; slot++)
        if (contacts[slot].active)
            return slot;
    return -1;
}

static int
pointer_slot(void)
{
    int found = -1;

    for (int slot = 0; slot < MAX_SLOTS; slot++)
        if (contacts[slot].active && (found < 0 || contacts[slot].order < contacts[found].order))
            found = slot;
    return found;
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
    if (pointer_slot() == slot) {
        queue(EV_ABS, ABS_X, x);
        queue(EV_ABS, ABS_Y, y);
    }
    c->emitted_x = x;
    c->emitted_y = y;
}

static int
set_timer(int64_t interval)
{
    struct itimerspec spec = { 0 };

    spec.it_interval.tv_sec = spec.it_value.tv_sec = interval / NSEC_PER_SEC;
    spec.it_interval.tv_nsec = spec.it_value.tv_nsec = interval % NSEC_PER_SEC;
    if (timerfd_settime(timer, 0, &spec, NULL) < 0) {
        perror("wallview-touch: timerfd_settime");
        return -1;
    }
    return 0;
}

static void
arm_phase(int64_t at)
{
    struct itimerspec spec = { 0 };

    spec.it_value.tv_sec = at / NSEC_PER_SEC;
    spec.it_value.tv_nsec = at % NSEC_PER_SEC;
    if (timerfd_settime(phase_timer, TFD_TIMER_ABSTIME, &spec, NULL) < 0)
        perror("wallview-touch: timerfd_settime");
}

static int
held_by_other(dev_t device)
{
    DIR *proc = opendir("/proc");
    struct dirent *entry;
    int found = 0;
    pid_t self = getpid();

    if (!proc)
        return 0;
    while (!found && (entry = readdir(proc))) {
        char *end;
        long pid = strtol(entry->d_name, &end, 10);

        if (*end || pid <= 0 || pid == self)
            continue;

        char dir[64];

        snprintf(dir, sizeof(dir), "/proc/%ld/fd", pid);
        DIR *fds = opendir(dir);
        struct dirent *fd;

        if (!fds)
            continue;
        while (!found && (fd = readdir(fds))) {
            char link[320];
            struct stat st;

            if (fd->d_name[0] == '.')
                continue;
            snprintf(link, sizeof(link), "%s/%s", dir, fd->d_name);
            found = stat(link, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == device;
        }
        closedir(fds);
    }
    closedir(proc);
    return found;
}

static void
open_drm(void)
{
    if (drm >= 0 || drm_failed || !opts || !opts->vblank || now_ns() < drm_retry_at)
        return;
    struct stat node;
    int64_t started = now_ns();

    drm_retry_at = started + drm_backoff;
    drm_backoff = drm_backoff * 2 < 60 * NSEC_PER_SEC ? drm_backoff * 2 : 60 * NSEC_PER_SEC;
    if (stat(opts->drm_path, &node) < 0 || !S_ISCHR(node.st_mode)) {
        if (!drm_warned)
            fprintf(stderr, "wallview-touch: %s is not a device node, using the timer\n", opts->drm_path);
        drm_warned = 1;
        return;
    }

    int held = held_by_other(node.st_rdev);

    if (opts->debug)
        fprintf(stderr, "wallview-touch: drm scan %.1f ms, held %d\n", (now_ns() - started) / 1e6, held);
    if (!held) {
        if (!drm_warned)
            fprintf(stderr, "wallview-touch: no other process holds %s, using the timer\n", opts->drm_path);
        drm_warned = 1;
        return;
    }

    int fd = open(opts->drm_path, O_RDWR | O_CLOEXEC | O_NONBLOCK);

    if (fd < 0) {
        if (!drm_warned)
            fprintf(stderr, "wallview-touch: %s: %s, using the timer\n", opts->drm_path, strerror(errno));
        drm_warned = 1;
        return;
    }
    if (ioctl(fd, DRM_IOCTL_DROP_MASTER, 0) == 0) {
        if (!drm_warned)
            fprintf(stderr, "wallview-touch: %s had no master, using the timer\n", opts->drm_path);
        drm_warned = 1;
        close(fd);
        return;
    }

    char name[32] = { 0 };
    struct drm_version version = { .name_len = sizeof(name) - 1, .name = name };

    if (ioctl(fd, DRM_IOCTL_VERSION, &version) < 0 || strcmp(name, opts->drm_driver)) {
        fprintf(stderr, "wallview-touch: %s is driver \"%s\", not %s, using the timer\n", opts->drm_path, name,
                opts->drm_driver);
        close(fd);
        drm_failed = 1;
        return;
    }
    drm = fd;
    drm_backoff = 5 * NSEC_PER_SEC;
}

static void
close_drm(void)
{
    if (drm >= 0)
        close(drm);
    drm = -1;
    vblank_pending = 0;
}

static int
request_vblank(void)
{
    if (vblank_pending)
        return 0;
    if (drm < 0 || (vblank_retry_at && now_ns() < vblank_retry_at))
        return -1;

    union drm_wait_vblank wait = { .request = { .type = _DRM_VBLANK_RELATIVE | _DRM_VBLANK_EVENT, .sequence = 1 } };

    if (ioctl(drm, DRM_IOCTL_WAIT_VBLANK, &wait) < 0) {
        vblank_retry_at = now_ns() + NSEC_PER_SEC;
        return -1;
    }
    vblank_retry_at = 0;
    vblank_pending = 1;
    return 0;
}

static void
arm_timer(int on)
{
    int mode = !on ? 0 : request_vblank() == 0 ? 2 : 1;

    if (mode == timer_armed)
        return;
    if (mode != 2 && phase_timer >= 0) {
        struct itimerspec off = { 0 };

        timerfd_settime(phase_timer, 0, &off, NULL);
    }
    if (set_timer(mode == 2 ? 3 * frame : mode == 1 ? period : 0) == 0)
        timer_armed = mode;
}

static int64_t
read_vblank(void)
{
    struct drm_event_vblank events[16];
    ssize_t size;
    int64_t latest = 0;

    while ((size = read(drm, events, sizeof(events))) > 0) {
        for (ssize_t i = 0; i < size / (ssize_t)sizeof(events[0]); i++) {
            if (events[i].base.type != DRM_EVENT_VBLANK)
                continue;

            int64_t time = events[i].tv_sec * NSEC_PER_SEC + events[i].tv_usec * 1000LL;
            int64_t gap = time - last_vblank;

            if (last_vblank && gap > frame / 2 && gap < frame * 3 / 2 && gap > 4 * NSEC_PER_MSEC &&
                gap < 50 * NSEC_PER_MSEC)
                frame = (7 * frame + gap) / 8;
            last_vblank = time;
            if (time > latest)
                latest = time;
        }
        if ((size_t)size < sizeof(events))
            break;
    }
    if (size < 0 && errno != EAGAIN && errno != EINTR) {
        close_drm();
        if (timer_armed == 2)
            arm_timer(1);
        return 0;
    }
    if (latest) {
        vblank_pending = 0;
        if (timer_armed == 2)
            set_timer(3 * frame);
    }
    return latest;
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
    if (n > 2 && *vx * (s[n - 1].x - s[n - 2].x) + *vy * (s[n - 1].y - s[n - 2].y) < 0) {
        double speed = sqrt(*vx * *vx + *vy * *vy);

        *vx = *vy = 0;
        return speed;
    }
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
    int first = pointer_slot() == slot;
    struct sample *last = c->samples ? &c->history[c->samples - 1] : NULL;
    struct sample *prev = c->samples > 1 ? &c->history[c->samples - 2] : NULL;
    int blended = c->blend || c->offset_x != 0 || c->offset_y != 0;
    double dx = 0, dy = 0;

    if (prev && c->coasting && (c->vx != 0 || c->vy != 0)) {
        dx = c->vx;
        dy = c->vy;
    } else if (prev) {
        dx = c->dir_x;
        dy = c->dir_y;
        if (dx == 0 && dy == 0)
            velocity(c, &dx, &dy);
    }
    int ahead = prev && c->emitted_x >= 0 && (c->emitted_x - last->x) * dx + (c->emitted_y - last->y) * dy > 0;

    c->releasing = 0;
    c->coasting = 0;
    int decelerated = c->fresh && !blended && prev &&
        (last->x - prev->x) * dx + (last->y - prev->y) * dy < (c->emitted_x - last->x) * dx + (c->emitted_y - last->y) * dy;

    if (last && (blended || c->predicted ? !ahead || decelerated : 1))
        emit_position(slot, clamp(last->x, min_x, max_x), clamp(last->y, min_y, max_y));
    c->fresh = 0;
    c->blend = 0;
    c->offset_x = c->offset_y = 0;
    for (int i = 0, s = -1; i < pending_count; i++) {
        if (pending[i].type == EV_ABS && pending[i].code == ABS_MT_SLOT)
            s = pending[i].value;
        else if (s == slot && pending[i].type == EV_ABS &&
                 (pending[i].code == ABS_MT_POSITION_X || pending[i].code == ABS_MT_POSITION_Y)) {
            flush();
            break;
        }
    }
    queue(EV_ABS, ABS_MT_SLOT, slot);
    queue(EV_ABS, ABS_MT_TRACKING_ID, -1);
    c->active = 0;
    c->dirty = 0;
    int next = pointer_slot();

    if (next >= 0 && first) {
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
    int tap = c->reach < opts->predict_travel;
    double ux = speed > 0 ? c->vx : last->x - c->origin_x;
    double uy = speed > 0 ? c->vy : last->y - c->origin_y;
    double norm = hypot(ux, uy);
    double along = norm > 0 && (speed > 0 || tap) ? ((c->x - last->x) * ux + (c->y - last->y) * uy) / norm : 0;
    double side = norm > 0 ? fabs((c->x - last->x) * uy - (c->y - last->y) * ux) / norm : hypot(c->x - last->x, c->y - last->y);
    int ok = dx * dx + dy * dy <= limit * limit &&
             (tap ? along >= -1 && side <= 4 + 0.5 * along : along >= -opts->rejoin_min);

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
    if (c->multi)
        return 0;
    for (int other = 0; other < MAX_SLOTS; other++)
        if (other != slot && contacts[other].active && !contacts[other].releasing)
            return 0;
    const struct sample *last = &c->history[c->samples - 1];
    double speed = velocity(c, &c->vx, &c->vy);
    const struct sample *before = &c->history[c->samples - 2];
    double move = hypot(last->x - before->x, last->y - before->y);

    c->turned = 0;
    if (speed > 0 && c->vx == 0 && c->vy == 0 && move >= opts->hold_travel) {
        double dt = (double)(last->time - before->time) / NSEC_PER_SEC;

        c->vx = (last->x - before->x) / dt;
        c->vy = (last->y - before->y) / dt;
        c->turned = 1;
    }
    int age_ok = time - last->time <= opts->hold_recent;
    int hold = age_ok && !(c->turned && c->reach < opts->predict_travel) && (opts->hold_max_speed <= 0 || speed <= opts->hold_max_speed) &&
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

static void resample(const struct options *options, int64_t now);

static void
handle_frame(int64_t time)
{
    int wake = 0;
    int landing = 0;

    for (int slot = 0; slot < MAX_SLOTS; slot++)
        landing |= contacts[slot].started && !contacts[slot].releasing;
    for (int slot = 0; slot < MAX_SLOTS; slot++) {
        struct contact *c = &contacts[slot];

        if (!c->ended || c->started)
            continue;
        wake = 1;
        c->ended = 0;
        if (!c->active || c->releasing) {
            c->dirty = 0;
        } else if (opts->lift_hold > 0 && !landing && holds(slot, time)) {
            c->releasing = 1;
            c->coasting = 0;
            c->lift_at = time;
            c->release_at = time + (c->reach < opts->predict_travel ? opts->tap_hold : opts->lift_hold);
            c->dirty = 0;
        } else {
            release(slot);
        }
    }
    for (int slot = 0; slot < MAX_SLOTS; slot++) {
        struct contact *c = &contacts[slot];

        if (c->started || c->ended || c->dirty)
            wake = 1;
        if (c->started && c->releasing && rejoins(slot, time)) {
            c->started = 0;
            c->releasing = 0;
            c->coasting = 0;
            c->dirty = 0;
            c->fresh = 1;
            c->blend = 1;
            if (c->vx != 0 || c->vy != 0) {
                c->dir_x = c->vx;
                c->dir_y = c->vy;
            }
            add_sample(c, time);
        } else if (c->started) {
            int held = c->active;

            for (int other = 0; other < MAX_SLOTS; other++)
                if (other != slot && contacts[other].releasing) {
                    release(other);
                    held = 1;
                }
            if (c->active)
                release(slot);
            if (held)
                flush();
            c->started = 0;
            c->active = 1;
            c->samples = 0;
            c->moved = 0;
            c->reach = 0;
            c->blend = 0;
            c->offset_x = c->offset_y = 0;
            c->dir_x = c->dir_y = 0;
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
            c->order = ++started_count;
            c->multi = 0;
            emit_position(slot, c->x, c->y);
            c->dirty = 0;
        } else if (c->dirty) {
            int64_t gap = c->samples ? time - c->history[c->samples - 1].time : 0;
            int last_x = c->samples ? c->history[c->samples - 1].x : c->x;
            int last_y = c->samples ? c->history[c->samples - 1].y : c->y;

            c->dirty = 0;
            c->fresh = 1;
            if (gap > opts->gap_blend_min && gap < opts->gap_blend &&
                (double)(c->x - last_x) * (c->x - last_x) + (double)(c->y - last_y) * (c->y - last_y) >
                    (double)opts->rejoin_min * opts->rejoin_min)
                c->blend = 1;
            double mx = c->x - last_x, my = c->y - last_y;

            int n = c->samples;
            double px = n >= 2 ? c->history[n - 1].x - c->history[n - 2].x : 0;
            double py = n >= 2 ? c->history[n - 1].y - c->history[n - 2].y : 0;

            if ((mx != 0 || my != 0) && (mx * c->dir_x + my * c->dir_y >= 0 || mx * mx + my * my > 16 || mx * px + my * py > 0)) {
                c->dir_x = mx;
                c->dir_y = my;
            }
            add_sample(c, time);
        }
    }
    int down = 0;

    for (int slot = 0; slot < MAX_SLOTS; slot++)
        down += contacts[slot].active && !contacts[slot].releasing;
    if (down > 1)
        for (int slot = 0; slot < MAX_SLOTS; slot++)
            if (contacts[slot].active && !contacts[slot].releasing)
                contacts[slot].multi = 1;
    flush();
    if (first_active() < 0)
        arm_timer(0);
    else if (wake && !timer_armed)
        resample(opts, time);
    else if (wake)
        arm_timer(1);
}

static int
interpolate(int a, int b, int64_t ta, int64_t tb, int64_t t)
{
    if (tb == ta)
        return b;
    double alpha = (double)(t - ta) / (double)(tb - ta);
    return round_half_away(a + (b - a) * alpha);
}

static int
coast(struct contact *c, const struct sample *last, int64_t target, int64_t step, int *x, int *y)
{
    double speed = sqrt(c->vx * c->vx + c->vy * c->vy);

    if (opts->coast <= 0 || c->turned || speed <= 0 || speed < opts->coast_speed || target <= last->time ||
        c->reach < opts->predict_travel ||
        c->lift_at - last->time > opts->coast_after)
        return 0;
    if (!c->coasting) {
        c->coasting = 1;
        if (c->emitted_x >= 0) {
            c->offset_x = c->offset_y = 0;
            c->coast_x = c->emitted_x;
            c->coast_y = c->emitted_y;
            c->coast_at = target - step;
        } else {
            c->coast_x = last->x;
            c->coast_y = last->y;
            c->coast_at = last->time;
        }
    }
    double tau = (double)opts->coast / NSEC_PER_SEC;
    double t = (double)(target - c->coast_at) / NSEC_PER_SEC;
    double distance = speed * tau * (1 - exp(-t / tau));

    int spent = distance >= opts->coast_max;

    if (spent)
        distance = opts->coast_max;
    *x = round_half_away(c->coast_x + c->vx / speed * distance);
    *y = round_half_away(c->coast_y + c->vy / speed * distance);
    return spent || *x < min_x || *x > max_x || *y < min_y || *y > max_y;
}

static int64_t
prediction_life(void)
{
    int64_t fade = 4 * (timer_armed == 2 ? frame : period);

    return opts->ease_min + fade > opts->max_sample_gap ? opts->ease_min + fade : opts->max_sample_gap;
}

static void
predict(const struct contact *c, const struct sample *last, int64_t target, int *x, int *y)
{
    const struct sample *a = &c->history[c->samples - 2];
    int64_t interval = last->time - a->time;
    int64_t age = target - last->time;
    int64_t ahead = age;
    int64_t limit = interval < opts->max_prediction ? interval : opts->max_prediction;
    int64_t ease_from = interval + interval / 2 > opts->ease_min ? interval + interval / 2 : opts->ease_min;
    int64_t fade = 4 * (timer_armed == 2 ? frame : period);
    int64_t life = prediction_life();

    if (ease_from > life - fade)
        ease_from = life - fade > 0 ? life - fade : 0;
    double vx, vy;

    if (interval > opts->max_sample_gap || age <= 0 || age > life || c->reach < opts->predict_travel)
        return;
    if (ahead > limit)
        ahead = limit;
    if (age > ease_from) {
        double keep = 1 - (double)(age - ease_from) / (double)fade;

        ahead = keep > 0 ? (int64_t)(ahead * keep) : 0;
    }
    velocity(c, &vx, &vy);
    *x = round_half_away(last->x + vx * ahead / NSEC_PER_SEC);
    *y = round_half_away(last->y + vy * ahead / NSEC_PER_SEC);
}

static void
resample(const struct options *options, int64_t now)
{
    int busy = 0;

    int64_t target = now - options->latency;

    expire_releases(now);

    for (int slot = 0; slot < MAX_SLOTS; slot++) {
        struct contact *c = &contacts[slot];

        if (!c->active || !c->samples)
            continue;

        struct sample *last = &c->history[c->samples - 1];
        int x = last->x;
        int y = last->y;

        int spent = 0;

        c->predicted = 0;
        if (target < c->history[0].time) {
            x = c->history[0].x;
            y = c->history[0].y;
        } else if (target < last->time) {
            for (int i = c->samples - 1; i > 0; i--) {
                struct sample *a = &c->history[i - 1];
                struct sample *b = &c->history[i];

                if (a->time <= target && target <= b->time) {
                    if (b->time - a->time <= options->max_sample_gap) {
                        x = interpolate(a->x, b->x, a->time, b->time, target);
                        y = interpolate(a->y, b->y, a->time, b->time, target);
                    } else {
                        x = b->x;
                        y = b->y;
                    }
                    break;
                }
            }
        } else {
            if (c->releasing) {
                spent = coast(c, last, target, timer_armed == 2 ? frame : opts->period, &x, &y);
                if (!c->coasting && c->emitted_x >= 0) {
                    x = c->emitted_x;
                    y = c->emitted_y;
                    c->offset_x = c->offset_y = 0;
                }
            } else if (c->samples >= 2)
                predict(c, last, target, &x, &y);
            c->predicted = x != last->x || y != last->y;
        }
        int64_t step = timer_armed == 2 ? frame : opts->period;
        double decay = step != opts->period ? pow(opts->blend, (double)step / opts->period) : opts->blend;

        if (c->blend) {
            c->blend = 0;
            if (c->emitted_x >= 0) {
                c->offset_x = (c->emitted_x - x) * decay;
                c->offset_y = (c->emitted_y - y) * decay;
            }
        }
        int blending = c->offset_x != 0 || c->offset_y != 0;

        if (blending) {
            x = round_half_away(x + c->offset_x);
            y = round_half_away(y + c->offset_y);
            c->offset_x *= decay;
            c->offset_y *= decay;
            if (c->offset_x > -0.5 && c->offset_x < 0.5 && c->offset_y > -0.5 && c->offset_y < 0.5)
                c->offset_x = c->offset_y = 0;
            else
                c->predicted = 1;
        }
        c->fresh = 0;
        emit_position(slot, clamp(x, min_x, max_x), clamp(y, min_y, max_y));
        if (spent)
            release(slot);
        else if (c->releasing || blending || target - last->time <= prediction_life())
            busy = 1;
    }
    flush();
    arm_timer(busy);
}

static int dropped;
static int input_fd = -1;

static int
read_slots(int code, int32_t *values)
{
    struct {
        uint32_t code;
        int32_t values[MAX_SLOTS];
    } slots = { .code = code };

    for (int slot = 0; slot < MAX_SLOTS; slot++)
        slots.values[slot] = code == ABS_MT_TRACKING_ID ? -1 : 0;
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

    for (int tries = 0;; tries++) {
        struct input_event late[64];

        if (ioctl(input_fd, EVIOCGABS(ABS_MT_SLOT), &current_slot) == 0 && current_slot.value >= 0 &&
            current_slot.value < MAX_SLOTS)
            *slot = current_slot.value;
        if (read_slots(ABS_MT_TRACKING_ID, ids) < 0)
            return;
        load_positions();
        if (input_fd >= 0)
            usleep(300);

        int32_t again[MAX_SLOTS], xs[MAX_SLOTS], ys[MAX_SLOTS];
        int settled = read_slots(ABS_MT_TRACKING_ID, again) == 0 && read_slots(ABS_MT_POSITION_X, xs) == 0 &&
                      read_slots(ABS_MT_POSITION_Y, ys) == 0 && !memcmp(again, ids, sizeof(ids));

        for (int i = 0; settled && i < MAX_SLOTS; i++)
            settled = ids[i] < 0 || (xs[i] == contacts[i].x && ys[i] == contacts[i].y);
        if (tries == 8 || ((input_fd < 0 || read(input_fd, late, sizeof(late)) <= 0) && settled))
            break;
        while (read(input_fd, late, sizeof(late)) > 0)
            ;
    }
    for (int slot = 0; slot < MAX_SLOTS; slot++) {
        struct contact *c = &contacts[slot];

        if (ids[slot] < 0) {
            c->started = 0;
            if (c->active && !c->releasing)
                c->ended = 1;
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
    struct contact *c = *slot >= 0 ? &contacts[*slot] : NULL;
    int64_t time = event->input_event_sec * NSEC_PER_SEC + event->input_event_usec * 1000LL;

    if (event->type == EV_SYN && event->code == SYN_DROPPED) {
        dropped = 1;
        return;
    }
    if (dropped)
        return;
    if (event->type == EV_ABS) {
        if (event->code == ABS_MT_SLOT) {
            *slot = event->value >= 0 && event->value < MAX_SLOTS ? event->value : -1;
            return;
        }
        if (!c)
            return;
        switch (event->code) {
        case ABS_MT_TRACKING_ID:
            if (event->value < 0) {
                c->ended = c->active || c->started;
                c->started = 0;
            } else if (!(c->active && !c->releasing && c->tracking_id == event->value)) {
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
    while (*end == ' ' || *end == '\n' || *end == '\t' || *end == '\r')
        end++;
    if (*end || !isfinite(number) || number < 0 || number > 1e6) {
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

    if (rate < 2 || rate > 1000) {
        fprintf(stderr, "wallview-touch: ignoring WALLVIEW_TOUCH_RATE outside 2-1000\n");
        rate = 120;
    }

    *options = (struct options){
        .period = NSEC_PER_SEC / rate,
        .latency = env_ms("WALLVIEW_TOUCH_LATENCY_MS", 16),
        .max_prediction = env_ms("WALLVIEW_TOUCH_PREDICTION_MS", 30),
        .max_sample_gap = env_ms("WALLVIEW_TOUCH_MAX_GAP_MS", 150),
        .lift_hold = env_ms("WALLVIEW_TOUCH_LIFT_MS", 80),
        .tap_hold = env_ms("WALLVIEW_TOUCH_TAP_LIFT_MS", -1),
        .rejoin_min = (int)env_number("WALLVIEW_TOUCH_REJOIN_PX", 30),
        .rejoin_slack = env_number("WALLVIEW_TOUCH_REJOIN_SLACK", 0.75),
        .hold_travel = (int)env_number("WALLVIEW_TOUCH_HOLD_TRAVEL_PX", 5),
        .start_travel = (int)env_number("WALLVIEW_TOUCH_START_TRAVEL_PX", 5),
        .hold_speed = env_number("WALLVIEW_TOUCH_HOLD_PX_PER_S", 60),
        .hold_max_speed = env_number("WALLVIEW_TOUCH_HOLD_MAX_PX_PER_S", 0),
        .start_speed = env_number("WALLVIEW_TOUCH_START_PX_PER_S", 150),
        .hold_recent = env_ms("WALLVIEW_TOUCH_HOLD_RECENT_MS", 100),
        .coast = env_ms("WALLVIEW_TOUCH_COAST_MS", 250),
        .coast_after = env_ms("WALLVIEW_TOUCH_COAST_AFTER_MS", 85),
        .coast_max = (int)env_number("WALLVIEW_TOUCH_COAST_PX", 80),
        .coast_speed = env_number("WALLVIEW_TOUCH_COAST_PX_PER_S", 150),
        .blend = env_number("WALLVIEW_TOUCH_BLEND", 0.7),
        .ease_min = env_ms("WALLVIEW_TOUCH_PREDICT_EASE_MS", 100),
        .gap_blend = env_ms("WALLVIEW_TOUCH_GAP_BLEND_MS", 280),
        .gap_blend_min = env_ms("WALLVIEW_TOUCH_GAP_BLEND_MIN_MS", 60),
        .predict_travel = (int)env_number("WALLVIEW_TOUCH_PREDICT_TRAVEL_PX", 13),
        .debug = (int)env_number("WALLVIEW_TOUCH_DEBUG", 0),
        .vblank = (int)env_number("WALLVIEW_TOUCH_VBLANK", 0),
        .drm_path = getenv("WALLVIEW_TOUCH_DRM") && *getenv("WALLVIEW_TOUCH_DRM") ? getenv("WALLVIEW_TOUCH_DRM") : "/dev/dri/card0",
        .drm_driver = getenv("WALLVIEW_TOUCH_DRM_DRIVER") && *getenv("WALLVIEW_TOUCH_DRM_DRIVER") ? getenv("WALLVIEW_TOUCH_DRM_DRIVER") : "rockchip",
        .vblank_offset = env_ms("WALLVIEW_TOUCH_VBLANK_OFFSET_MS", 10),
    };
    if (options->tap_hold < 0 || options->tap_hold > options->lift_hold)
        options->tap_hold = options->lift_hold;
    if (options->blend > 0.95) {
        fprintf(stderr, "wallview-touch: ignoring WALLVIEW_TOUCH_BLEND above 0.95\n");
        options->blend = 0.7;
    }
}

static volatile sig_atomic_t stopping;

static void
stop(int signal)
{
    (void)signal;
    stopping = 1;
}

static int
touching(int input)
{
    unsigned long keys[(KEY_CNT + 63) / 64] = { 0 };

    return ioctl(input, EVIOCGKEY(sizeof(keys)), keys) == 0 && (keys[BTN_TOUCH / 64] & (1ul << (BTN_TOUCH % 64)));
}

static int
finish(int status)
{
    if (output >= 0) {
        for (int slot = 0; slot < MAX_SLOTS; slot++)
            if (contacts[slot].active)
                release(slot);
        flush();
        ioctl(output, UI_DEV_DESTROY);
    }
    return status;
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
        if (waited < 600 && touching(input)) {
            if (waited == 100)
                fprintf(stderr, "wallview-touch: touch held for 5 s, waiting for the lift\n");
            usleep(50000);
            continue;
        }
        if (ioctl(input, EVIOCGRAB, 1) < 0) {
            fprintf(stderr, "wallview-touch: grab: %s\n", strerror(errno));
            return 1;
        }
        if (waited >= 600) {
            fprintf(stderr, "wallview-touch: touch held for 30 s, grabbing anyway\n");
            break;
        }
        if (!touching(input))
            break;
        ioctl(input, EVIOCGRAB, 0);
        usleep(50000);
    }
    int slot = 0;

    output = create_output(input);
    if (output < 0) {
        fprintf(stderr, "wallview-touch: uinput: %s\n", strerror(errno));
        return 1;
    }

    struct sched_param param = { .sched_priority = 40 };

    if (sched_setscheduler(0, SCHED_FIFO, &param) < 0)
        fprintf(stderr, "wallview-touch: SCHED_FIFO: %s\n", strerror(errno));
    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
        fprintf(stderr, "wallview-touch: mlockall: %s\n", strerror(errno));

    sigset_t blocked, unblocked;

    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGINT);
    sigprocmask(SIG_BLOCK, &blocked, &unblocked);

    struct sigaction action = { .sa_handler = stop };

    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    period = options.period;
    opts = &options;
    timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (timer < 0) {
        fprintf(stderr, "wallview-touch: timerfd: %s\n", strerror(errno));
        return 1;
    }

    phase_timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (phase_timer < 0) {
        fprintf(stderr, "wallview-touch: timerfd: %s\n", strerror(errno));
        return 1;
    }

    struct pollfd fds[4] = { { input, POLLIN, 0 }, { timer, POLLIN, 0 }, { -1, POLLIN, 0 }, { phase_timer, POLLIN, 0 } };

    {
        struct input_event stale[64];

        while (read(input, stale, sizeof(stale)) > 0)
            ;
    }
    resync(now_ns(), &slot);
    open_drm();
    while (!stopping) {
        fds[2].fd = drm;
        if (ppoll(fds, 4, NULL, &unblocked) < 0) {
            if (errno == EINTR)
                continue;
            return finish(1);
        }
        if (fds[0].revents & (POLLERR | POLLHUP))
            return finish(1);
        if (fds[0].revents & POLLIN) {
            struct input_event events[64];
            ssize_t size;

            while ((size = read(input, events, sizeof(events))) > 0) {
                for (size_t i = 0; i < (size_t)size / sizeof(events[0]); i++)
                    handle_event(&events[i], &slot);
                if ((size_t)size < sizeof(events))
                    break;
            }
            if (dropped) {
                while (read(input, events, sizeof(events)) > 0)
                    ;
                dropped = 0;
                resync(now_ns(), &slot);
            }
        }
        int handled = 0;

        if (drm >= 0 && (fds[2].revents & (POLLERR | POLLHUP | POLLNVAL)))
            close_drm();
        else if (drm >= 0 && (fds[2].revents & POLLIN)) {
            int64_t vblank = read_vblank();

            if (vblank) {
                int64_t at = vblank + options.vblank_offset;
                int64_t now = now_ns();

                handled = 1;
                if (options.vblank_offset > 0 && options.vblank_offset < frame && at > now)
                    arm_phase(at);
                else
                    resample(&options, now);
            }
        }
        if (fds[3].revents & POLLIN) {
            uint64_t expirations;

            if (read(phase_timer, &expirations, sizeof(expirations)) > 0 && !handled) {
                handled = 1;
                resample(&options, now_ns());
            }
        }
        if (fds[1].revents & POLLIN) {
            uint64_t expirations;

            if (read(timer, &expirations, sizeof(expirations)) > 0 && !handled) {
                if (timer_armed == 2)
                    vblank_pending = 0;
                resample(&options, now_ns());
            }
        }
        if (first_active() < 0)
            open_drm();
    }
    return finish(0);
}

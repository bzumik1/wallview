#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define MAX_PLANES 16

static int
all_planes(int fd, uint32_t *ids)
{
    drmModePlaneResPtr planes = drmModeGetPlaneResources(fd);
    int count = 0;

    for (uint32_t i = 0; planes && i < planes->count_planes && count < MAX_PLANES; i++)
        ids[count++] = planes->planes[i];
    drmModeFreePlaneResources(planes);
    return count;
}

static int
frame_changed(int fd, const uint32_t *ids, uint32_t *last, int count, int *first)
{
    int changed = 0;

    for (int i = 0; i < count; i++) {
        drmModePlanePtr p = drmModeGetPlane(fd, ids[i]);
        uint32_t fb = p ? p->fb_id : 0;

        if (fb && fb != last[i])
            changed = 1;
        last[i] = fb;
        drmModeFreePlane(p);
    }
    if (*first) {
        *first = 0;
        return 0;
    }
    return changed;
}

int
main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    uint32_t ids[MAX_PLANES], last[MAX_PLANES] = { 0 };
    int count, first = 1;
    int vblanks = 0, flips = 0, idle_run = 0, longest_gap = 0, gaps[8] = { 0 };

    if (fd >= 0)
        drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (fd < 0 || !(count = all_planes(fd, ids))) {
        fprintf(stderr, "no planes\n");
        return 1;
    }
    for (int total = seconds * 60; vblanks < total; vblanks++) {
        drmVBlank vbl = { .request = { .type = DRM_VBLANK_RELATIVE, .sequence = 1 } };

        if (drmWaitVBlank(fd, &vbl))
            return 1;
        if (frame_changed(fd, ids, last, count, &first)) {
            if (getenv("FLIPMON_TIMES"))
                fprintf(stderr, "flip %ld.%06ld\n", (long)vbl.reply.tval_sec, (long)vbl.reply.tval_usec);
            flips++;
            gaps[idle_run < 7 ? idle_run : 7]++;
            if (idle_run > longest_gap)
                longest_gap = idle_run;
            idle_run = 0;
        } else {
            idle_run++;
        }
    }
    printf("{\"vblanks\":%d,\"flips\":%d,\"longestGapVblanks\":%d,\"gapHistogram\":[%d,%d,%d,%d,%d,%d,%d,%d]}\n",
           vblanks, flips, longest_gap, gaps[0], gaps[1], gaps[2], gaps[3], gaps[4], gaps[5], gaps[6], gaps[7]);
    return 0;
}

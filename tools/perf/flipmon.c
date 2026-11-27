#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static uint32_t
primary_plane(int fd)
{
    drmModePlaneResPtr planes = drmModeGetPlaneResources(fd);
    uint32_t found = 0;

    for (uint32_t i = 0; planes && i < planes->count_planes && !found; i++) {
        drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, planes->planes[i], DRM_MODE_OBJECT_PLANE);
        for (uint32_t j = 0; props && j < props->count_props; j++) {
            drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[j]);
            if (prop && !strcmp(prop->name, "type") && props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY)
                found = planes->planes[i];
            drmModeFreeProperty(prop);
        }
        drmModeFreeObjectProperties(props);
    }
    drmModeFreePlaneResources(planes);
    return found;
}

int
main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    uint32_t plane, last_fb = 0;
    int vblanks = 0, flips = 0, idle_run = 0, longest_gap = 0, gaps[8] = { 0 };

    if (fd >= 0)
        drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (fd < 0 || !(plane = primary_plane(fd))) {
        fprintf(stderr, "no primary plane\n");
        return 1;
    }
    for (int total = seconds * 60; vblanks < total; vblanks++) {
        drmVBlank vbl = { .request = { .type = DRM_VBLANK_RELATIVE, .sequence = 1 } };
        drmModePlanePtr p;

        if (drmWaitVBlank(fd, &vbl))
            return 1;
        p = drmModeGetPlane(fd, plane);
        if (p && p->fb_id != last_fb) {
            if (last_fb) {
                flips++;
                gaps[idle_run < 7 ? idle_run : 7]++;
                if (idle_run > longest_gap)
                    longest_gap = idle_run;
            }
            last_fb = p->fb_id;
            idle_run = 0;
        } else {
            idle_run++;
        }
        drmModeFreePlane(p);
    }
    printf("{\"vblanks\":%d,\"flips\":%d,\"longestGapVblanks\":%d,\"gapHistogram\":[%d,%d,%d,%d,%d,%d,%d,%d]}\n",
           vblanks, flips, longest_gap, gaps[0], gaps[1], gaps[2], gaps[3], gaps[4], gaps[5], gaps[6], gaps[7]);
    return 0;
}

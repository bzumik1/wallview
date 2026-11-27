#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fd;

static void
emit(int type, int code, int value)
{
    struct input_event ev = { .type = type, .code = code, .value = value };
    if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
        exit(1);
}

static void
sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void
setup_abs(int code, int max)
{
    struct uinput_abs_setup abs = { .code = code, .absinfo = { .minimum = 0, .maximum = max } };
    ioctl(fd, UI_ABS_SETUP, &abs);
}

static void
contact(int x, int y, int down)
{
    if (down) {
        emit(EV_ABS, ABS_MT_SLOT, 0);
        emit(EV_ABS, ABS_MT_TRACKING_ID, 1);
    }
    emit(EV_ABS, ABS_MT_POSITION_X, x);
    emit(EV_ABS, ABS_MT_POSITION_Y, y);
    emit(EV_ABS, ABS_X, x);
    emit(EV_ABS, ABS_Y, y);
    if (down)
        emit(EV_KEY, BTN_TOUCH, 1);
    emit(EV_SYN, SYN_REPORT, 0);
}

static void
release(void)
{
    emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit(EV_KEY, BTN_TOUCH, 0);
    emit(EV_SYN, SYN_REPORT, 0);
}

static void
swipe(int x, int y0, int y1, int duration_ms)
{
    int steps = duration_ms / 8;

    contact(x, y0, 1);
    for (int i = 1; i <= steps; i++) {
        sleep_ms(8);
        contact(x, y0 + (y1 - y0) * i / steps, 0);
    }
    release();
}

int
main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "scroll";
    int repeat = argc > 2 ? atoi(argv[2]) : 4;
    struct uinput_setup setup = { .id = { .bustype = BUS_VIRTUAL, .vendor = 0x1, .product = 0x1 } };

    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return 1;
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
    setup_abs(ABS_X, 479);
    setup_abs(ABS_Y, 479);
    setup_abs(ABS_MT_SLOT, 0);
    setup_abs(ABS_MT_TRACKING_ID, 65535);
    setup_abs(ABS_MT_POSITION_X, 479);
    setup_abs(ABS_MT_POSITION_Y, 479);
    strcpy(setup.name, "wallview-touchplay");
    ioctl(fd, UI_DEV_SETUP, &setup);
    ioctl(fd, UI_DEV_CREATE);
    sleep(2);

    for (int i = 0; i < repeat; i++) {
        if (!strcmp(mode, "tap")) {
            contact(240, 240, 1);
            sleep_ms(60);
            release();
            sleep_ms(1500);
        } else {
            swipe(240, 400, 120, 250);
            sleep_ms(1200);
            swipe(240, 120, 400, 250);
            sleep_ms(1200);
        }
    }
    sleep(1);
    ioctl(fd, UI_DEV_DESTROY);
    return 0;
}

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define ADDR 0x5a
#define I2C_DEV "/dev/i2c-1"
#define GPIOCHIP "/dev/gpiochip0"
#define GPIOCHIP_LABEL "gpio0"
#define RESET_LINE 12
#define DRIVER_LINK "/sys/bus/i2c/drivers/Hynitron-TS/1-005a"
#define IMAGE_SIZE 7680
#define PAGE_SIZE 512
#define VENDOR_CHECKSUM 0x99630e41u

static int i2c;
static int reset_fd = -1;

static const uint8_t ident[20] = {
    0x0d, 0x00, 0x08, 0x00,
    0xe0, 0x01, 0xe0, 0x01,
    0xdc, 0x5f, 0xca, 0xca,
    0x02, 0x03, 0x00, 0x00,
    0x01, 0x4b, 0xa8, 0x00,
};

static void
sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

static uint32_t
le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int
xfer(struct i2c_msg *m, int n)
{
    struct i2c_rdwr_ioctl_data d = { m, (uint32_t)n };

    return ioctl(i2c, I2C_RDWR, &d) == n ? 0 : -1;
}

static int
wr(const uint8_t *buf, int len)
{
    struct i2c_msg m = { ADDR, 0, (uint16_t)len, (uint8_t *)buf };

    return xfer(&m, 1);
}

static int
rd(uint16_t reg, uint8_t *buf, int len)
{
    uint8_t r[2] = { reg >> 8, reg & 0xff };
    struct i2c_msg m = { ADDR, I2C_M_RD, (uint16_t)len, buf };

    if (wr(r, 2))
        return -1;
    return xfer(&m, 1);
}

static int
w3(uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t buf[3] = { a, b, c };

    return wr(buf, 3);
}

static int
wr5(const uint8_t *buf, int len)
{
    for (int i = 0; i < 5; i++)
        if (wr(buf, len) == 0)
            return 0;
    return -1;
}

static int
rd5(uint16_t reg, uint8_t *buf, int len)
{
    for (int i = 0; i < 5; i++)
        if (rd(reg, buf, len) == 0)
            return 0;
    return -1;
}

static int
w3r(uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t buf[3] = { a, b, c };

    return wr5(buf, 3);
}

static int
reset_pulse(int wait_ms)
{
    struct gpiohandle_data v = { 0 };

    if (ioctl(reset_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &v) < 0)
        return -1;
    sleep_ms(10);
    v.values[0] = 1;
    if (ioctl(reset_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &v) < 0)
        return -1;
    sleep_ms(wait_ms);
    return 0;
}

static int
open_reset(void)
{
    struct gpiochip_info chipinfo = { 0 };
    struct gpioline_info line = { .line_offset = RESET_LINE };
    struct gpiohandle_request req = {
        .lineoffsets = { RESET_LINE },
        .flags = GPIOHANDLE_REQUEST_OUTPUT,
        .default_values = { 1 },
        .lines = 1,
    };
    int chip = open(GPIOCHIP, O_RDONLY);
    int rc = -1;

    if (chip < 0) {
        perror("cstfw: " GPIOCHIP);
        return -1;
    }
    if (ioctl(chip, GPIO_GET_CHIPINFO_IOCTL, &chipinfo) < 0 || strcmp(chipinfo.label, GPIOCHIP_LABEL)) {
        fprintf(stderr, "cstfw: %s is \"%s\", expected \"%s\"; reset line not touched\n", GPIOCHIP,
                chipinfo.label[0] ? chipinfo.label : "?", GPIOCHIP_LABEL);
        goto out;
    }
    if (ioctl(chip, GPIO_GET_LINEINFO_IOCTL, &line) < 0 || (line.flags & GPIOLINE_FLAG_KERNEL)) {
        fprintf(stderr, "cstfw: reset line %d is in use (\"%s\"); is the driver still bound?\n", RESET_LINE,
                line.consumer);
        goto out;
    }
    strcpy(req.consumer_label, "cstfw-reset");
    if (ioctl(chip, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        perror("cstfw: reset line");
        goto out;
    }
    reset_fd = req.fd;
    rc = 0;
out:
    close(chip);
    return rc;
}

static int
info_read(uint8_t *out, int print)
{
    static const uint16_t regs[] = { 0xd1f4, 0xd1f8, 0xd1fc, 0xd200, 0xd204, 0xd208, 0xd20c };
    static const uint8_t start[2] = { 0xd1, 0x01 }, end[2] = { 0xd1, 0x09 };
    int rc = 0;

    if (wr(start, 2)) {
        perror("cstfw: info mode");
        return 1;
    }
    sleep_ms(10);
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint8_t b[4] = { 0 };

        if (rd(regs[i], b, 4)) {
            if (print)
                printf("%04x ERR\n", regs[i]);
            rc = 1;
            continue;
        }
        if (print)
            printf("%04x %02x %02x %02x %02x\n", regs[i], b[0], b[1], b[2], b[3]);
        if (out)
            memcpy(out + 4 * i, b, 4);
    }
    if (wr(end, 2))
        rc = 1;
    fflush(stdout);
    return rc;
}

static int
enter_boot(void)
{
    uint8_t v = 0;

    if (reset_pulse(12))
        return -1;
    for (int i = 0; i < 30; i++) {
        int sent = w3(0xa0, 0x01, 0xaa) == 0;

        sleep_ms(6);
        if (!sent) {
            printf("try %d: A0 01 AA not acknowledged\n", i + 1);
            continue;
        }
        if (rd(0xa003, &v, 1)) {
            printf("try %d: 0xA003 read failed\n", i + 1);
            continue;
        }
        if (v == 0x55)
            return 0;
        printf("try %d: 0xA003 = %02x\n", i + 1, v);
    }
    return -1;
}

static int
boot_check(int read_checksum)
{
    int ok;

    if (open_reset())
        return 1;
    ok = enter_boot() == 0;
    printf("boot mode: %s\n", ok ? "entered (0xA003 = 0x55)" : "NOT entered");
    if (ok && read_checksum) {
        uint8_t a = 0, b[4] = { 0 };
        int have = rd(0xa000, &a, 1) == 0;

        printf("a000 %s%02x\n", have ? "" : "ERR ", a);
        if (have && a == 0x01 && rd(0xa008, b, 4) == 0)
            printf("a008 %02x %02x %02x %02x (checksum 0x%08x, vendor image 0x%08x)\n", b[0], b[1], b[2], b[3],
                   le32(b), VENDOR_CHECKSUM);
        else
            printf("no checksum available from the boot loader before programming\n");
    }
    if (reset_pulse(60))
        return 1;
    printf("after reset:\n");
    return info_read(NULL, 1) || !ok;
}

static uint32_t
image_checksum(const uint8_t *img)
{
    uint32_t sum = 0x55;

    for (int i = 0; i < 0x1dfc; i += 4)
        sum += le32(img + i);
    return sum;
}

static int
load_image(const char *path, uint8_t *img)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) {
        perror(path);
        return -1;
    }
    n = fread(img, 1, IMAGE_SIZE + 1, f);
    fclose(f);
    if (n != IMAGE_SIZE) {
        fprintf(stderr, "cstfw: %s: %zu bytes, expected %d\n", path, n, IMAGE_SIZE);
        return -1;
    }
    if (memcmp(img + 0x1de4, ident, sizeof(ident))) {
        fprintf(stderr, "cstfw: %s: trailer does not identify a CST226SE 13x8 480x480 project 0x4b01 image\n", path);
        return -1;
    }
    if (image_checksum(img) != le32(img + 0x1dfc)) {
        fprintf(stderr, "cstfw: %s: checksum 0x%08x does not match trailer 0x%08x\n", path, image_checksum(img),
                le32(img + 0x1dfc));
        return -1;
    }
    return 0;
}

static int
flash_once(const uint8_t *img)
{
    uint8_t v = 0, b[4] = { 0 };
    int ok;

    if (enter_boot()) {
        fprintf(stderr, "cstfw: boot mode not entered, nothing erased in this attempt\n");
        return -2;
    }
    if (w3r(0xa0, 0x06, 0x00))
        return -1;
    for (int page = 0; page < IMAGE_SIZE / PAGE_SIZE; page++) {
        int offset = page * PAGE_SIZE;
        uint8_t sel[4] = { 0xa0, 0x14, offset & 0xff, offset >> 8 };

        if (wr5(sel, 4))
            return -1;
        for (int k = 0; k < PAGE_SIZE / 4; k++) {
            uint16_t reg = 0xa018 + 4 * k;
            uint8_t data[6] = { reg >> 8, reg & 0xff };

            memcpy(data + 2, img + offset + 4 * k, 4);
            if (wr5(data, 6))
                return -1;
        }
        if (w3r(0xa0, 0x04, 0xee))
            return -1;
        sleep_ms(600);
        ok = 0;
        for (int i = 0; i < 10 && !ok; i++) {
            ok = rd5(0xa005, &v, 1) == 0 && v == 0x55;
            if (!ok)
                sleep_ms(100);
        }
        if (!ok) {
            fprintf(stderr, "cstfw: page %d not acknowledged (0xA005 = %02x)\n", page, v);
            return -1;
        }
        printf("page %2d/%d written\n", page + 1, IMAGE_SIZE / PAGE_SIZE);
        fflush(stdout);
    }
    if (w3r(0xa0, 0x01, 0x00) || w3r(0xa0, 0x03, 0x00))
        return -1;
    sleep_ms(10);
    ok = 0;
    for (int i = 0; i < 10 && !ok; i++) {
        ok = rd5(0xa000, &v, 1) == 0 && v == 0x01;
        sleep_ms(4);
    }
    if (!ok) {
        fprintf(stderr, "cstfw: no checksum from the boot loader (0xA000 = %02x)\n", v);
        return -1;
    }
    if (rd5(0xa008, b, 4)) {
        fprintf(stderr, "cstfw: checksum read failed\n");
        return -1;
    }
    if (le32(b) != le32(img + 0x1dfc)) {
        fprintf(stderr, "cstfw: checksum 0x%08x, image 0x%08x\n", le32(b), le32(img + 0x1dfc));
        return -1;
    }
    if (w3r(0xa0, 0x06, 0xee))
        fprintf(stderr, "cstfw: A0 06 EE not acknowledged (continuing with reset)\n");
    sleep_ms(100);
    if (reset_pulse(30))
        return -1;
    return 0;
}

static int
flash(const char *path, int recover, uint32_t current)
{
    static uint8_t img[IMAGE_SIZE + 1];
    uint8_t chip[28];

    if (load_image(path, img))
        return 2;
    printf("image %s: version 0x%08x, checksum 0x%08x%s\n", path, le32(img + 0x1df8), le32(img + 0x1dfc),
           le32(img + 0x1dfc) == VENDOR_CHECKSUM ? " (vendor image)" : " (MODIFIED image)");
    if (recover) {
        if (le32(img + 0x1dfc) != VENDOR_CHECKSUM) {
            fprintf(stderr, "cstfw: --recover takes only the vendor image (checksum 0x%08x)\n", VENDOR_CHECKSUM);
            return 2;
        }
        printf("recover: chip not read first\n");
        if (open_reset())
            return 2;
    } else {
        if (open_reset())
            return 2;
        printf("chip before:\n");
        if (info_read(chip, 1)) {
            fprintf(stderr, "cstfw: chip info not readable; refusing to flash (use --recover with the vendor image)\n");
            return 3;
        }
        if (memcmp(chip, ident, sizeof(ident))) {
            fprintf(stderr, "cstfw: chip identification differs from the image; refusing to flash\n");
            return 3;
        }
        if (le32(chip + 24) != current) {
            fprintf(stderr, "cstfw: chip checksum 0x%08x, --current-checksum 0x%08x; refusing to flash\n",
                    le32(chip + 24), current);
            return 3;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    struct sched_param param = { .sched_priority = 50 };

    if (sched_setscheduler(0, SCHED_FIFO, &param))
        printf("SCHED_FIFO not set: %s\n", strerror(errno));
    if (mlockall(MCL_CURRENT | MCL_FUTURE))
        printf("mlockall failed: %s\n", strerror(errno));

    int erased = 0;

    for (int attempt = 1; attempt <= 5; attempt++) {
        int r;

        printf("attempt %d\n", attempt);
        fflush(stdout);
        r = flash_once(img);
        if (r == 0) {
            static const int waits[] = { 60, 200, 500 };

            printf("programmed and verified by the boot loader; chip after:\n");
            for (int i = 0; i < 3; i++) {
                if (i > 0 && reset_pulse(0))
                    break;
                sleep_ms(waits[i]);
                if (info_read(chip, 1) == 0 && memcmp(chip, ident, sizeof(ident)) == 0 &&
                    le32(chip + 24) == le32(img + 0x1dfc)) {
                    printf("cstfw: done and verified, checksum 0x%08x\n", le32(chip + 24));
                    return 0;
                }
            }
            fprintf(stderr, "cstfw: the new application does not answer as expected\n");
            return 4;
        }
        if (r != -2)
            erased = 1;
    }
    if (!erased) {
        fprintf(stderr, "cstfw: boot mode was never reached, nothing written\n");
        return 5;
    }
    fprintf(stderr, "cstfw: FAILED after 5 attempts. Do not power off and do not bind the driver;\n"
                    "       flash the vendor image with --recover.\n");
    return 1;
}

int
main(int argc, char **argv)
{
    int is_flash = argc == 6 && !strcmp(argv[1], "flash") && !strcmp(argv[3], "--current-checksum") &&
                   !strcmp(argv[5], "--erase-and-program");
    int is_recover = argc == 5 && !strcmp(argv[1], "flash") && !strcmp(argv[3], "--recover") &&
                     !strcmp(argv[4], "--erase-and-program");
    unsigned long current = 0;
    int simple = argc == 2 && (!strcmp(argv[1], "info") || !strcmp(argv[1], "boot-check") ||
                               !strcmp(argv[1], "boot-checksum"));

    if (!is_flash && !is_recover && !simple) {
        fprintf(stderr, "usage: cstfw info|boot-check|boot-checksum\n"
                        "       cstfw flash <image> --current-checksum <0x...> --erase-and-program\n"
                        "       cstfw flash <vendor image> --recover --erase-and-program\n");
        return 2;
    }
    if (is_flash) {
        char *end;

        errno = 0;
        current = strtoul(argv[4], &end, 16);
        if (strncmp(argv[4], "0x", 2) || end == argv[4] + 2 || *end || errno || current > 0xffffffffUL) {
            fprintf(stderr, "cstfw: --current-checksum must be 0x followed by up to 8 hex digits\n");
            return 2;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (access(DRIVER_LINK, F_OK) == 0) {
        fprintf(stderr, "cstfw: driver still bound, unbind first: echo 1-005a > /sys/bus/i2c/drivers/Hynitron-TS/unbind\n");
        return 2;
    }
    i2c = open(I2C_DEV, O_RDWR);
    if (i2c < 0) {
        perror("cstfw: " I2C_DEV);
        return 2;
    }
    if (is_flash || is_recover)
        return flash(argv[2], is_recover, (uint32_t)current);
    if (!strcmp(argv[1], "info"))
        return info_read(NULL, 1);
    return boot_check(!strcmp(argv[1], "boot-checksum"));
}

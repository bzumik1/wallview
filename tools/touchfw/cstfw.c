/*
 * Read-only access to the CST226SE touch controller (see docs/touch-controller.md).
 *
 *   cstfw info        firmware trailer registers in info mode (version, checksum, ...)
 *   cstfw boot-check  reset, enter the boot loader (A0 01 AA), read 0xA003 (expect 0x55),
 *                     read 0xA000/0xA008 (checksum, if the boot loader reports one), reset
 *   cstfw flash <image> --erase-and-program
 *                     program a 7680-byte image with the vendor protocol
 *
 * info and boot-check never write the controller's flash. flash does, and is
 * the only command that can leave the controller without a working application:
 * use it only after boot-check succeeded, on the development panel, with the
 * owner's approval (docs/touch-controller.md, plan (a) and (b)). Run every
 * command with the kernel driver unbound (tools/touchfw/panel-cstfw.sh).
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define ADDR 0x5a
#define I2C_DEV "/dev/i2c-1"
#define GPIOCHIP "/dev/gpiochip0"
#define RESET_LINE 12 /* GPIO0_B4, active low */
#define DRIVER_LINK "/sys/bus/i2c/drivers/Hynitron-TS/1-005a"

static int i2c;
static int reset_fd = -1;

static void
sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
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
    struct i2c_msg m[2] = { { ADDR, 0, 2, r }, { ADDR, I2C_M_RD, (uint16_t)len, buf } };

    return xfer(m, 2);
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
    int chip = open(GPIOCHIP, O_RDONLY);
    struct gpiohandle_request req = {
        .lineoffsets = { RESET_LINE },
        .flags = GPIOHANDLE_REQUEST_OUTPUT,
        .default_values = { 1 },
        .lines = 1,
    };

    strcpy(req.consumer_label, "cstfw-reset");
    if (chip < 0 || ioctl(chip, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        perror("cstfw: reset line");
        return -1;
    }
    close(chip);
    reset_fd = req.fd;
    return 0;
}

static int
info(void)
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
            printf("%04x ERR\n", regs[i]);
            rc = 1;
            continue;
        }
        printf("%04x %02x %02x %02x %02x\n", regs[i], b[0], b[1], b[2], b[3]);
    }
    if (wr(end, 2))
        rc = 1;
    return rc;
}

static int
boot_check(void)
{
    static const uint8_t enter[3] = { 0xa0, 0x01, 0xaa };
    uint8_t b[4] = { 0 };
    int ok = 0;

    if (open_reset())
        return 1;
    /* Vendor timing: reset, wait 10-20 ms, A0 01 AA, 6 ms, check 0xA003; up to 30 tries. */
    if (reset_pulse(12))
        return 1;
    for (int i = 0; i < 30 && !ok; i++) {
        if (wr(enter, 3) == 0) {
            sleep_ms(6);
            if (rd(0xa003, b, 1) == 0 && b[0] == 0x55)
                ok = 1;
        } else {
            sleep_ms(6);
        }
        if (!ok)
            printf("try %d: 0xA003 = %02x\n", i + 1, b[0]);
    }
    printf("boot mode: %s\n", ok ? "entered (0xA003 = 0x55)" : "NOT entered");
    if (ok) {
        if (rd(0xa000, b, 1) == 0)
            printf("a000 %02x\n", b[0]);
        else
            printf("a000 ERR\n");
        memset(b, 0, sizeof(b));
        if (rd(0xa008, b, 4) == 0)
            printf("a008 %02x %02x %02x %02x (checksum 0x%08x, image 0x99630e41)\n", b[0], b[1], b[2], b[3],
                   (unsigned)(b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24));
        else
            printf("a008 ERR\n");
    }
    /* Leave boot mode by reset only; A0 06 EE (exit after programming) is not needed. */
    if (reset_pulse(60))
        return 1;
    printf("after reset:\n");
    return info() || !ok;
}

#define IMAGE_SIZE 7680
#define PAGE_SIZE 512

static uint32_t
le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Checksum = 0x55 + sum of the 1919 little-endian words before the checksum word. */
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
    /* Trailer fields that identify this controller and panel; any other image is refused. */
    static const uint8_t ident[] = {
        0x0d, 0x00, 0x08, 0x00, /* 0x1de4: 13 TX, 8 RX */
        0xe0, 0x01, 0xe0, 0x01, /* 0x1de8: 480 x 480 */
        0xdc, 0x5f, 0xca, 0xca, /* 0x1dec: check code */
        0x02, 0x03, 0x00, 0x00, /* 0x1df0 */
        0x01, 0x4b, 0xa8, 0x00, /* 0x1df4: project 0x4b01, chip 0x00a8 */
    };
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
w3(uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t buf[3] = { a, b, c };

    return wr(buf, 3);
}

static int
read1(uint16_t reg, uint8_t *v)
{
    return rd(reg, v, 1);
}

/* One attempt of the vendor update sequence (cst2xx_check_code), from reset to reset. */
static int
flash_once(const uint8_t *img)
{
    uint8_t v = 0, b[4];
    int ok = 0;

    if (reset_pulse(12))
        return -1;
    for (int i = 0; i < 30 && !ok; i++) {
        if (w3(0xa0, 0x01, 0xaa) == 0) {
            sleep_ms(6);
            ok = read1(0xa003, &v) == 0 && v == 0x55;
        } else {
            sleep_ms(6);
        }
    }
    if (!ok) {
        fprintf(stderr, "cstfw: boot mode not entered, nothing erased\n");
        return -1;
    }
    if (w3(0xa0, 0x06, 0x00))
        return -1;
    for (int page = 0; page < IMAGE_SIZE / PAGE_SIZE; page++) {
        int offset = page * PAGE_SIZE;
        uint8_t sel[4] = { 0xa0, 0x14, offset & 0xff, offset >> 8 };

        if (wr(sel, 4))
            return -1;
        for (int k = 0; k < PAGE_SIZE / 4; k++) {
            uint16_t reg = 0xa018 + 4 * k;
            uint8_t data[6] = { reg >> 8, reg & 0xff };

            memcpy(data + 2, img + offset + 4 * k, 4);
            if (wr(data, 6))
                return -1;
        }
        if (w3(0xa0, 0x04, 0xee))
            return -1;
        sleep_ms(600);
        ok = 0;
        for (int i = 0; i < 10 && !ok; i++) {
            ok = read1(0xa005, &v) == 0 && v == 0x55;
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
    if (w3(0xa0, 0x01, 0x00) || w3(0xa0, 0x03, 0x00))
        return -1;
    sleep_ms(10);
    ok = 0;
    for (int i = 0; i < 10 && !ok; i++) {
        ok = read1(0xa000, &v) == 0 && v == 0x01;
        sleep_ms(4);
    }
    if (!ok) {
        fprintf(stderr, "cstfw: no checksum from the boot loader (0xA000 = %02x)\n", v);
        return -1;
    }
    if (rd(0xa008, b, 4) || le32(b) != le32(img + 0x1dfc)) {
        fprintf(stderr, "cstfw: checksum 0x%08x, image 0x%08x\n", le32(b), le32(img + 0x1dfc));
        return -1;
    }
    w3(0xa0, 0x06, 0xee);
    sleep_ms(100);
    if (reset_pulse(30))
        return -1;
    return 0;
}

static int
flash(const char *path)
{
    static uint8_t img[IMAGE_SIZE + 1];

    if (load_image(path, img) || open_reset())
        return 1;
    printf("image %s: version 0x%08x, checksum 0x%08x\n", path, le32(img + 0x1df8), le32(img + 0x1dfc));
    /* Like the vendor driver: on any failure start again from reset and page 0, up to 5 times. */
    for (int attempt = 1; attempt <= 5; attempt++) {
        printf("attempt %d\n", attempt);
        if (flash_once(img) == 0) {
            sleep_ms(60);
            printf("programmed and verified; info:\n");
            return info();
        }
    }
    fprintf(stderr, "cstfw: FAILED after 5 attempts. Do not power off; run the same flash command again.\n");
    return 1;
}

int
main(int argc, char **argv)
{
    int is_flash = argc == 4 && !strcmp(argv[1], "flash") && !strcmp(argv[3], "--erase-and-program");

    if (!is_flash && (argc != 2 || (strcmp(argv[1], "info") && strcmp(argv[1], "boot-check")))) {
        fprintf(stderr, "usage: cstfw info|boot-check\n       cstfw flash <image> --erase-and-program\n");
        return 2;
    }
    if (access(DRIVER_LINK, F_OK) == 0) {
        fprintf(stderr, "cstfw: driver still bound, unbind first: echo 1-005a > /sys/bus/i2c/drivers/Hynitron-TS/unbind\n");
        return 2;
    }
    i2c = open(I2C_DEV, O_RDWR);
    if (i2c < 0) {
        perror("cstfw: " I2C_DEV);
        return 1;
    }
    if (is_flash)
        return flash(argv[2]);
    return strcmp(argv[1], "info") == 0 ? info() : boot_check();
}

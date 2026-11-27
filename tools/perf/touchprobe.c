#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define TOUCH_ADDR 0x5a

static int i2c;

static uint64_t
now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static int
transfer(struct i2c_msg *msgs, int count)
{
    struct i2c_rdwr_ioctl_data data = { msgs, count };

    return ioctl(i2c, I2C_RDWR, &data) == count ? 0 : -1;
}

static int
read_touch(uint8_t *buf)
{
    uint8_t reg[2] = { 0xd0, 0x00 };
    struct i2c_msg msgs[2] = {
        { TOUCH_ADDR, 0, sizeof(reg), reg },
        { TOUCH_ADDR, I2C_M_RD, 28, buf },
    };

    return transfer(msgs, 2);
}

static int
finish_read(void)
{
    uint8_t cmd[3] = { 0xd0, 0x00, 0xab };
    struct i2c_msg msg = { TOUCH_ADDR, 0, sizeof(cmd), cmd };

    return transfer(&msg, 1);
}

static void
sample(int acknowledge)
{
    uint8_t buf[28];
    uint64_t start = now_ns();

    if (read_touch(buf)) {
        printf("E %llu\n", (unsigned long long)start);
        return;
    }
    if (acknowledge)
        finish_read();
    printf("D %llu %llu %u %u %u %u %u\n", (unsigned long long)start, (unsigned long long)now_ns(),
           buf[5] & 0x7f, (buf[0] & 0x0f) >> 1, (buf[1] << 4) | (buf[3] >> 4), (buf[2] << 4) | (buf[3] & 0x0f),
           buf[6]);
}

static void
frame_counter(void)
{
    uint8_t reg[2] = { 0xd0, 0x40 };
    uint8_t buf[6];
    struct i2c_msg msgs[2] = {
        { TOUCH_ADDR, 0, sizeof(reg), reg },
        { TOUCH_ADDR, I2C_M_RD, sizeof(buf), buf },
    };
    uint64_t start = now_ns();

    if (transfer(msgs, 2)) {
        printf("CE %llu\n", (unsigned long long)start);
        return;
    }
    uint32_t count = (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 | buf[3];
    unsigned sum = (buf[0] + buf[1] + buf[2] + buf[3] + 0xa5) & 0xffff;
    printf("C %llu %u %d\n", (unsigned long long)start, count, sum == (unsigned)(buf[4] << 8 | buf[5]));
}

int
main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: touchprobe <i2c-dev> <gpiochip> <line> <seconds> [level|poll<ms>] [ack|noack] [counter<ms>]\n");
        return 1;
    }

    const char *mode = argc > 5 ? argv[5] : "level";
    int acknowledge = argc > 6 ? strcmp(argv[6], "noack") != 0 : 1;
    int counter_ms = argc > 7 && !strncmp(argv[7], "counter", 7) ? atoi(argv[7] + 7) : 0;
    uint64_t next_counter = now_ns();
    uint64_t end = now_ns() + strtoull(argv[4], NULL, 10) * 1000000000ull;

    i2c = open(argv[1], O_RDWR);
    if (i2c < 0 || ioctl(i2c, I2C_SLAVE, TOUCH_ADDR) < 0) {
        perror("i2c");
        return 1;
    }

    int chip = open(argv[2], O_RDONLY);
    struct gpiohandle_request request = {
        .lineoffsets = { (uint32_t)atoi(argv[3]) },
        .flags = GPIOHANDLE_REQUEST_INPUT,
        .lines = 1,
    };
    strcpy(request.consumer_label, "touchprobe");
    if (chip < 0 || ioctl(chip, GPIO_GET_LINEHANDLE_IOCTL, &request) < 0) {
        perror("gpio");
        return 1;
    }

    setvbuf(stdout, NULL, _IOFBF, 1 << 20);
    int poll_ms = !strncmp(mode, "poll", 4) ? atoi(mode + 4) : 0;
    uint64_t next_poll = now_ns();
    int level = 1;

    while (now_ns() < end) {
        struct gpiohandle_data values;

        if (ioctl(request.fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &values) == 0 && values.values[0] != level) {
            level = values.values[0];
            printf("%c %llu\n", level ? 'R' : 'F', (unsigned long long)now_ns());
            if (!level && !poll_ms)
                sample(acknowledge);
        }
        if (poll_ms && now_ns() >= next_poll) {
            sample(acknowledge);
            next_poll += (uint64_t)poll_ms * 1000000ull;
        }
        if (counter_ms > 0 && now_ns() >= next_counter) {
            frame_counter();
            next_counter += (uint64_t)counter_ms * 1000000ull;
        }
        usleep(100);
    }
    fflush(stdout);
    return 0;
}

#include "power_key.h"
#include "event_bus.h"
#include "gpio_hub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#ifndef KEY_POWER
#define KEY_POWER 116
#endif

static pthread_t g_thread;
static volatile int g_running = 0;
static int g_fd = -1;
static uint64_t g_press_us = 0;

/* 在 /dev/input/event* 里找 name 含 gpio-keys 或 能上报 KEY_POWER 的那个 */
static int open_power_key_device(void) {
    DIR *d = opendir("/dev/input");
    if (!d) return -1;
    struct dirent *ent;
    int best = -1;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;

        char name[64] = {0};
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);

        unsigned long keybits[(KEY_MAX / (8 * sizeof(unsigned long))) + 1];
        memset(keybits, 0, sizeof(keybits));
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0) {
            int has_power = !!(keybits[KEY_POWER / (8 * sizeof(unsigned long))] &
                               (1UL << (KEY_POWER % (8 * sizeof(unsigned long)))));
            if (has_power) {
                printf("[power_key] use %s (name=\"%s\")\n", path, name);
                best = fd;
                break;
            }
        }
        close(fd);
    }
    closedir(d);
    return best;
}

static void *reader_thread(void *arg) {
    (void)arg;
    struct input_event ev;
    while (g_running) {
        ssize_t n = read(g_fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev)) {
            if (errno == EINTR) continue;
            if (!g_running) break;
            usleep(20 * 1000);
            continue;
        }
        if (ev.type != EV_KEY || ev.code != KEY_POWER) continue;

        uint64_t now_us = event_bus_now_us();
        gpio_event_payload_t p = {0};
        p.gpio = POWER_KEY_GPIO;
        p.level = ev.value ? 1 : 0;
        p.timestamp_us = now_us;

        if (ev.value == 1) {            /* 按下 */
            p.type = GPIO_EVT_PRESS;
            p.press_duration_ms = 0;
            g_press_us = now_us;
            printf("[power_key] PRESS\n");
            event_bus_publish("gpio/press", &p, sizeof(p));
        } else if (ev.value == 0) {     /* 抬起 */
            p.type = GPIO_EVT_RELEASE;
            p.press_duration_ms = g_press_us ? (int)((now_us - g_press_us) / 1000) : 0;
            event_bus_publish("gpio/release", &p, sizeof(p));
        }
    }
    return NULL;
}

int power_key_init(void) {
    if (g_running) return 0;
    g_fd = open_power_key_device();
    if (g_fd < 0) {
        fprintf(stderr, "[power_key] no input device with KEY_POWER\n");
        return -1;
    }
    g_running = 1;
    if (pthread_create(&g_thread, NULL, reader_thread, NULL) != 0) {
        g_running = 0;
        close(g_fd);
        g_fd = -1;
        return -1;
    }
    return 0;
}

void power_key_shutdown(void) {
    if (!g_running) return;
    g_running = 0;
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    pthread_join(g_thread, NULL);
}

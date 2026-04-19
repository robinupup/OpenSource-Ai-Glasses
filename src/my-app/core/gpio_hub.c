/*
 * gpio_hub.c — sysfs 轮询 GPIO 事件中心（ai-core GPIO Hub 的开源复刻）
 *
 * 实测结论（AI Glasses / RV1103B，right temple=GPIO0, left temple=GPIO75）:
 *   - 触摸板 1/2 对应 GPIO 0 / 75，active_low（按下瞬间从 hi → lo）
 *   - 触摸脉冲宽度几十毫秒级，本文件 20ms 轮询 + 持久 fd lseek/read 足以捕获
 *   - **不需要 ai-core / guard / touchpad_manager 常驻做 I2C 初始化**；
 *     触摸板芯片由内核驱动在上电时已经配好，用户态只需 sysfs 读 value。
 *   - sysfs 里 /sys/class/gpio/gpioN/value 与 /sys/kernel/debug/gpio 的值同步
 *     （debug 接口仅作为 touchpad_manager 的 fallback，此处不需要）。
 */
#include "gpio_hub.h"
#include "event_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

#define GPIO_HUB_MAX        16
#define GPIO_POLL_INTERVAL  20 /* ms */
#define GPIO_SYSFS_ROOT     "/sys/class/gpio"

typedef struct {
    int           gpio;
    int           active_high;   /* 1=高电平视为按下 */
    int           is_pressed;    /* 当前按下态 */
    int           raw_level;     /* 最近一次读到的电平 */
    int           fd_value;      /* 持久打开 /sys/class/gpio/gpioN/value */
    uint64_t      press_start_us;
    int           in_use;
} gpio_slot_t;

typedef struct {
    gpio_slot_t    slots[GPIO_HUB_MAX];
    pthread_mutex_t mutex;
    pthread_t      poll_thread;
    volatile int   running;
    int            initialized;
} gpio_hub_t;

static gpio_hub_t g;

/* -------------------- sysfs 辅助 -------------------- */
static int write_file(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    int n = (int)write(fd, val, strlen(val));
    close(fd);
    return (n < 0) ? -1 : 0;
}

static int export_gpio(int gpio) {
    char path[64];
    snprintf(path, sizeof(path), GPIO_SYSFS_ROOT "/gpio%d", gpio);
    if (access(path, F_OK) == 0) return 0; /* already exported */
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", gpio);
    return write_file(GPIO_SYSFS_ROOT "/export", buf);
}

static int set_direction_in(int gpio) {
    char path[96];
    snprintf(path, sizeof(path), GPIO_SYSFS_ROOT "/gpio%d/direction", gpio);
    return write_file(path, "in");
}

static int open_value_fd(int gpio) {
    char path[96];
    snprintf(path, sizeof(path), GPIO_SYSFS_ROOT "/gpio%d/value", gpio);
    return open(path, O_RDONLY);
}

static int read_level(int fd) {
    if (fd < 0) return -1;
    char c = 0;
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    if (read(fd, &c, 1) != 1) return -1;
    return (c == '1') ? 1 : 0;
}

/* -------------------- 内部实现 -------------------- */
static gpio_slot_t *find_slot(int gpio) {
    for (int i = 0; i < GPIO_HUB_MAX; i++)
        if (g.slots[i].in_use && g.slots[i].gpio == gpio) return &g.slots[i];
    return NULL;
}

static gpio_slot_t *alloc_slot(void) {
    for (int i = 0; i < GPIO_HUB_MAX; i++)
        if (!g.slots[i].in_use) return &g.slots[i];
    return NULL;
}

static void publish_event(int gpio, gpio_evt_type_t type, int level, int press_ms) {
    gpio_event_payload_t p = {
        .gpio = gpio,
        .type = (int)type,
        .level = level,
        .press_duration_ms = press_ms,
        .timestamp_us = event_bus_now_us(),
    };
    /* 广播到通用主题 */
    const char *generic = (type == GPIO_EVT_PRESS) ? "gpio/press"
                        : (type == GPIO_EVT_RELEASE) ? "gpio/release"
                        : "gpio/error";
    event_bus_publish(generic, &p, sizeof(p));

    /* 也广播到特定 GPIO 主题，订阅者可精细筛选 */
    char topic[48];
    const char *sub = (type == GPIO_EVT_PRESS) ? "press"
                    : (type == GPIO_EVT_RELEASE) ? "release"
                    : "error";
    snprintf(topic, sizeof(topic), "gpio/%d/%s", gpio, sub);
    event_bus_publish(topic, &p, sizeof(p));
}

static void *poll_thread_fn(void *arg) {
    (void)arg;
    while (g.running) {
        pthread_mutex_lock(&g.mutex);
        for (int i = 0; i < GPIO_HUB_MAX; i++) {
            gpio_slot_t *s = &g.slots[i];
            if (!s->in_use) continue;
            int lvl = read_level(s->fd_value);
            if (lvl < 0) continue;
            s->raw_level = lvl;
            int pressed_now = s->active_high ? (lvl == 1) : (lvl == 0);
            if (pressed_now && !s->is_pressed) {
                s->is_pressed = 1;
                s->press_start_us = event_bus_now_us();
                publish_event(s->gpio, GPIO_EVT_PRESS, lvl, 0);
            } else if (!pressed_now && s->is_pressed) {
                int ms = (int)((event_bus_now_us() - s->press_start_us) / 1000ULL);
                s->is_pressed = 0;
                publish_event(s->gpio, GPIO_EVT_RELEASE, lvl, ms);
            }
        }
        pthread_mutex_unlock(&g.mutex);
        usleep(GPIO_POLL_INTERVAL * 1000);
    }
    return NULL;
}

/* -------------------- 公共 API -------------------- */
int gpio_hub_init(void) {
    if (g.initialized) return 0;
    memset(&g, 0, sizeof(g));
    pthread_mutex_init(&g.mutex, NULL);
    g.running = 1;
    if (pthread_create(&g.poll_thread, NULL, poll_thread_fn, NULL) != 0) {
        pthread_mutex_destroy(&g.mutex);
        return -1;
    }
    g.initialized = 1;
    return 0;
}

int gpio_hub_add(int gpio, gpio_active_t active) {
    if (!g.initialized) return -1;
    pthread_mutex_lock(&g.mutex);
    if (find_slot(gpio)) { pthread_mutex_unlock(&g.mutex); return 0; }
    gpio_slot_t *s = alloc_slot();
    if (!s) { pthread_mutex_unlock(&g.mutex); return -1; }

    if (export_gpio(gpio) != 0) {
        fprintf(stderr, "[gpio_hub] export GPIO%d failed: %s\n", gpio, strerror(errno));
    }
    if (set_direction_in(gpio) != 0) {
        fprintf(stderr, "[gpio_hub] set_direction_in GPIO%d failed\n", gpio);
    }
    int fd = open_value_fd(gpio);
    if (fd < 0) {
        fprintf(stderr, "[gpio_hub] open value fd GPIO%d failed: %s\n", gpio, strerror(errno));
        pthread_mutex_unlock(&g.mutex);
        return -1;
    }
    s->in_use = 1;
    s->gpio = gpio;
    s->active_high = (active == GPIO_ACTIVE_HIGH) ? 1 : 0;
    s->fd_value = fd;
    s->is_pressed = 0;
    s->raw_level = read_level(fd);
    pthread_mutex_unlock(&g.mutex);
    printf("[gpio_hub] Monitoring GPIO%d (active_%s)\n", gpio, s->active_high ? "high" : "low");
    return 0;
}

int gpio_hub_remove(int gpio) {
    if (!g.initialized) return -1;
    pthread_mutex_lock(&g.mutex);
    gpio_slot_t *s = find_slot(gpio);
    if (!s) { pthread_mutex_unlock(&g.mutex); return -1; }
    if (s->fd_value >= 0) close(s->fd_value);
    memset(s, 0, sizeof(*s));
    pthread_mutex_unlock(&g.mutex);
    return 0;
}

int gpio_hub_get_state(int gpio) {
    if (!g.initialized) return -1;
    pthread_mutex_lock(&g.mutex);
    gpio_slot_t *s = find_slot(gpio);
    int ret = -1;
    if (s) ret = s->is_pressed ? 1 : 0;
    pthread_mutex_unlock(&g.mutex);
    return ret;
}

void gpio_hub_shutdown(void) {
    if (!g.initialized) return;
    g.running = 0;
    pthread_join(g.poll_thread, NULL);
    for (int i = 0; i < GPIO_HUB_MAX; i++) {
        if (g.slots[i].in_use && g.slots[i].fd_value >= 0)
            close(g.slots[i].fd_value);
    }
    pthread_mutex_destroy(&g.mutex);
    g.initialized = 0;
}

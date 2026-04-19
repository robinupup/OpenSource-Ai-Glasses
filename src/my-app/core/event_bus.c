#include "event_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

typedef struct {
    int              id;
    int              active;
    char             pattern[EVENT_BUS_TOPIC_MAX];
    event_callback_t cb;
    void            *user_data;
    uint32_t         last_sequence;
} subscriber_t;

typedef struct {
    event_t          queue[EVENT_BUS_QUEUE_SIZE];
    volatile uint32_t write_index;
    volatile uint32_t global_sequence;

    subscriber_t     subs[EVENT_BUS_MAX_SUBSCRIBERS];
    int              next_sub_id;

    pthread_mutex_t  mutex;
    pthread_cond_t   cond;

    pthread_t        dispatcher;
    volatile int     running;
    int              initialized;
} event_bus_t;

static event_bus_t g_bus;

uint64_t event_bus_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* 通配匹配：支持按 '/' 分段的 '*' 匹配。'*' 仅匹配单段。 */
static int topic_match(const char *pattern, const char *topic) {
    while (*pattern && *topic) {
        if (*pattern == '*') {
            pattern++;
            while (*topic && *topic != '/') topic++;
            if (*pattern == '\0') return *topic == '\0';
            if (*pattern != '/')  return 0;
        } else {
            if (*pattern != *topic) return 0;
            pattern++;
            topic++;
        }
    }
    return *pattern == '\0' && *topic == '\0';
}

static void *dispatcher_thread(void *arg) {
    (void)arg;
    uint32_t read_index = 0;

    while (g_bus.running) {
        pthread_mutex_lock(&g_bus.mutex);
        while (g_bus.running && read_index == g_bus.write_index) {
            pthread_cond_wait(&g_bus.cond, &g_bus.mutex);
        }
        if (!g_bus.running) {
            pthread_mutex_unlock(&g_bus.mutex);
            break;
        }

        /* 若生产者已覆盖过我们未读的事件，快速跳到最新窗口 */
        if (g_bus.write_index - read_index > EVENT_BUS_QUEUE_SIZE) {
            read_index = g_bus.write_index - EVENT_BUS_QUEUE_SIZE;
        }

        event_t evt = g_bus.queue[read_index % EVENT_BUS_QUEUE_SIZE];
        read_index++;

        /* 复制订阅者快照，避免回调中修改列表导致死锁 */
        subscriber_t snapshot[EVENT_BUS_MAX_SUBSCRIBERS];
        int n = 0;
        for (int i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; i++) {
            if (g_bus.subs[i].active && topic_match(g_bus.subs[i].pattern, evt.topic)) {
                snapshot[n++] = g_bus.subs[i];
            }
        }
        pthread_mutex_unlock(&g_bus.mutex);

        for (int i = 0; i < n; i++) {
            if (snapshot[i].cb) snapshot[i].cb(&evt, snapshot[i].user_data);
        }
    }
    return NULL;
}

int event_bus_init(void) {
    if (g_bus.initialized) return 0;
    memset(&g_bus, 0, sizeof(g_bus));
    pthread_mutex_init(&g_bus.mutex, NULL);
    pthread_cond_init(&g_bus.cond, NULL);
    g_bus.next_sub_id = 1;
    g_bus.running = 1;
    if (pthread_create(&g_bus.dispatcher, NULL, dispatcher_thread, NULL) != 0) {
        pthread_mutex_destroy(&g_bus.mutex);
        pthread_cond_destroy(&g_bus.cond);
        return -1;
    }
    g_bus.initialized = 1;
    return 0;
}

void event_bus_shutdown(void) {
    if (!g_bus.initialized) return;
    pthread_mutex_lock(&g_bus.mutex);
    g_bus.running = 0;
    pthread_cond_broadcast(&g_bus.cond);
    pthread_mutex_unlock(&g_bus.mutex);
    pthread_join(g_bus.dispatcher, NULL);
    pthread_mutex_destroy(&g_bus.mutex);
    pthread_cond_destroy(&g_bus.cond);
    g_bus.initialized = 0;
}

int event_bus_subscribe(const char *topic_pattern, event_callback_t cb, void *user_data) {
    if (!g_bus.initialized || !topic_pattern || !cb) return -1;
    pthread_mutex_lock(&g_bus.mutex);
    int slot = -1;
    for (int i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; i++) {
        if (!g_bus.subs[i].active) { slot = i; break; }
    }
    if (slot < 0) { pthread_mutex_unlock(&g_bus.mutex); return -1; }
    g_bus.subs[slot].id = g_bus.next_sub_id++;
    g_bus.subs[slot].active = 1;
    strncpy(g_bus.subs[slot].pattern, topic_pattern, EVENT_BUS_TOPIC_MAX - 1);
    g_bus.subs[slot].pattern[EVENT_BUS_TOPIC_MAX - 1] = '\0';
    g_bus.subs[slot].cb = cb;
    g_bus.subs[slot].user_data = user_data;
    g_bus.subs[slot].last_sequence = g_bus.global_sequence;
    int id = g_bus.subs[slot].id;
    pthread_mutex_unlock(&g_bus.mutex);
    return id;
}

void event_bus_unsubscribe(int subscriber_id) {
    if (!g_bus.initialized) return;
    pthread_mutex_lock(&g_bus.mutex);
    for (int i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; i++) {
        if (g_bus.subs[i].active && g_bus.subs[i].id == subscriber_id) {
            g_bus.subs[i].active = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_bus.mutex);
}

int event_bus_publish(const char *topic, const void *payload, size_t payload_size) {
    if (!g_bus.initialized || !topic) return -1;
    if (payload_size > EVENT_BUS_PAYLOAD_MAX) return -1;

    pthread_mutex_lock(&g_bus.mutex);
    event_t *slot = &g_bus.queue[g_bus.write_index % EVENT_BUS_QUEUE_SIZE];
    memset(slot, 0, sizeof(*slot));
    strncpy(slot->topic, topic, EVENT_BUS_TOPIC_MAX - 1);
    slot->sequence    = ++g_bus.global_sequence;
    slot->timestamp_us = event_bus_now_us();
    slot->payload_size = payload_size;
    if (payload && payload_size) memcpy(slot->payload, payload, payload_size);
    g_bus.write_index++;
    pthread_cond_signal(&g_bus.cond);
    pthread_mutex_unlock(&g_bus.mutex);
    return 0;
}

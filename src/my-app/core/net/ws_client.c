#include "ws_client.h"
#include "mongoose.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

/* 发送请求队列条目 */
typedef struct send_item {
    struct send_item *next;
    int   is_binary;
    size_t len;
    uint8_t data[];
} send_item_t;

struct ws_client {
    struct mg_mgr         mgr;
    struct mg_connection *conn;           /* mongoose ws connection */
    char                  url[256];
    pthread_t             poll_thread;
    pthread_mutex_t       tx_mutex;
    send_item_t          *tx_head;
    send_item_t          *tx_tail;

    volatile int          running;
    volatile int          is_open;

    ws_msg_cb_t           on_msg;
    void                 *user_data;
};

/* ---------- 内部：发送队列 ---------- */
static void tx_push(ws_client_t *c, int is_binary, const void *buf, size_t len) {
    send_item_t *it = (send_item_t *)malloc(sizeof(*it) + len);
    if (!it) return;
    it->next = NULL;
    it->is_binary = is_binary;
    it->len = len;
    memcpy(it->data, buf, len);

    pthread_mutex_lock(&c->tx_mutex);
    if (c->tx_tail) {
        c->tx_tail->next = it;
        c->tx_tail = it;
    } else {
        c->tx_head = c->tx_tail = it;
    }
    pthread_mutex_unlock(&c->tx_mutex);
}

static send_item_t *tx_pop(ws_client_t *c) {
    pthread_mutex_lock(&c->tx_mutex);
    send_item_t *it = c->tx_head;
    if (it) {
        c->tx_head = it->next;
        if (!c->tx_head) c->tx_tail = NULL;
    }
    pthread_mutex_unlock(&c->tx_mutex);
    return it;
}

static void tx_drain_and_send(ws_client_t *c) {
    if (!c->conn || !c->is_open) return;
    send_item_t *it;
    while ((it = tx_pop(c)) != NULL) {
        int op = it->is_binary ? WEBSOCKET_OP_BINARY : WEBSOCKET_OP_TEXT;
        mg_ws_send(c->conn, it->data, it->len, op);
        free(it);
    }
}

/* ---------- mongoose 事件处理 ---------- */
static void fn(struct mg_connection *mc, int ev, void *ev_data) {
    ws_client_t *c = (ws_client_t *)mc->fn_data;
    if (!c) return;

    if (ev == MG_EV_OPEN) {
        /* TCP 建立，但 WS 握手还没完成 */
    } else if (ev == MG_EV_CONNECT) {
        /* TCP 连上（mongoose 会自动发 WS 握手请求） */
    } else if (ev == MG_EV_WS_OPEN) {
        c->is_open = 1;
        if (c->on_msg) c->on_msg(WS_EV_OPEN, WS_MSG_TEXT, NULL, 0, c->user_data);
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
        ws_msg_type_t type = (wm->flags & 0x0F) == WEBSOCKET_OP_BINARY
                                 ? WS_MSG_BINARY
                                 : WS_MSG_TEXT;
        if (c->on_msg) {
            c->on_msg(WS_EV_MSG, type, wm->data.buf, wm->data.len, c->user_data);
        }
    } else if (ev == MG_EV_ERROR) {
        const char *err = (const char *)ev_data;
        fprintf(stderr, "[ws_client] error: %s\n", err ? err : "?");
        if (c->on_msg) c->on_msg(WS_EV_ERROR, WS_MSG_TEXT, err,
                                  err ? strlen(err) : 0, c->user_data);
    } else if (ev == MG_EV_CLOSE) {
        c->is_open = 0;
        c->conn = NULL;
        if (c->on_msg) c->on_msg(WS_EV_CLOSE, WS_MSG_TEXT, NULL, 0, c->user_data);
    }
}

/* ---------- 后台 poll 线程 ---------- */
static void *poll_thread_main(void *arg) {
    ws_client_t *c = (ws_client_t *)arg;
    /* 发起连接 */
    c->conn = mg_ws_connect(&c->mgr, c->url, fn, c, NULL);
    if (!c->conn) {
        fprintf(stderr, "[ws_client] mg_ws_connect %s failed\n", c->url);
        c->running = 0;
        return NULL;
    }

    while (c->running) {
        mg_mgr_poll(&c->mgr, 50);   /* 50ms */
        tx_drain_and_send(c);
    }
    /* 清理 */
    if (c->conn) {
        c->conn->is_closing = 1;
        mg_mgr_poll(&c->mgr, 50);
    }
    mg_mgr_free(&c->mgr);
    return NULL;
}

/* ---------- 对外 API ---------- */
ws_client_t *ws_client_open(const char *url) {
    if (!url || !*url) return NULL;
    /* 默认 mg_log_level=MG_LL_DEBUG 会把每次 poll 的 write_conn 全打出来，
     * 录音时 PCM 每 40ms 一帧 → 日志洪泛，占 IO 又掩盖真日志。全局只留 ERROR。*/
    mg_log_set(MG_LL_ERROR);

    ws_client_t *c = (ws_client_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    pthread_mutex_init(&c->tx_mutex, NULL);
    snprintf(c->url, sizeof(c->url), "%s", url);
    mg_mgr_init(&c->mgr);
    c->running = 1;
    if (pthread_create(&c->poll_thread, NULL, poll_thread_main, c) != 0) {
        mg_mgr_free(&c->mgr);
        pthread_mutex_destroy(&c->tx_mutex);
        free(c);
        return NULL;
    }
    return c;
}

void ws_client_set_on_msg(ws_client_t *c, ws_msg_cb_t cb, void *user_data) {
    if (!c) return;
    c->on_msg = cb;
    c->user_data = user_data;
}

int ws_client_send_text(ws_client_t *c, const char *text) {
    if (!c || !text) return -1;
    tx_push(c, 0, text, strlen(text));
    return 0;
}

int ws_client_send_binary(ws_client_t *c, const void *data, size_t len) {
    if (!c || !data || len == 0) return -1;
    tx_push(c, 1, data, len);
    return 0;
}

int ws_client_is_open(ws_client_t *c) { return c ? c->is_open : 0; }

void ws_client_close(ws_client_t *c) {
    if (!c) return;
    c->running = 0;
    pthread_join(c->poll_thread, NULL);
    /* 清空未发送的 tx 队列 */
    send_item_t *it;
    while ((it = tx_pop(c)) != NULL) free(it);
    pthread_mutex_destroy(&c->tx_mutex);
    free(c);
}

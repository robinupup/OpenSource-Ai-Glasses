/**
 * my-app core/net - WebSocket 客户端封装（基于 mongoose）
 *
 * 用法：
 *   ws_client_t *c = ws_client_open("ws://192.168.1.100:8004/immersive_english");
 *   ws_client_set_on_msg(c, on_msg, ud);
 *   ws_client_send_text(c, "{\"type\":\"mic\",\"value\":\"off\"}");
 *   ws_client_send_binary(c, pcm, pcm_size);
 *   ws_client_close(c);
 *
 * - 内部起一条 pthread 跑 mg_mgr_poll，所有 mg_ws_* 调用都在那条线程里执行。
 * - 外部线程通过内部互斥队列把发送请求排入，不会并发访问 mongoose manager。
 * - 接收回调在 mongoose 线程触发，调用方需注意线程安全（拷贝数据后返回）。
 */

#ifndef MYAPP_WS_CLIENT_H
#define MYAPP_WS_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ws_client ws_client_t;

typedef enum {
    WS_MSG_TEXT   = 1,
    WS_MSG_BINARY = 2,
} ws_msg_type_t;

typedef enum {
    WS_EV_OPEN      = 1,  /* 握手完成，可以开始发送 */
    WS_EV_MSG       = 2,  /* 收到 text 或 binary */
    WS_EV_CLOSE     = 3,  /* 连接关闭 */
    WS_EV_ERROR     = 4,  /* 出错 */
} ws_event_t;

typedef void (*ws_msg_cb_t)(ws_event_t ev,
                             ws_msg_type_t type,
                             const void *data,
                             size_t len,
                             void *user_data);

/** 建立 WebSocket 连接（异步；open 成功会回调 WS_EV_OPEN）。 */
ws_client_t *ws_client_open(const char *url);

/** 注册接收/事件回调。回调运行在 mongoose 线程里。 */
void ws_client_set_on_msg(ws_client_t *c, ws_msg_cb_t cb, void *user_data);

/** 发送 JSON 文本帧。返回 0 成功，<0 失败。线程安全。 */
int ws_client_send_text(ws_client_t *c, const char *text);

/** 发送二进制帧（如 PCM）。返回 0 成功，<0 失败。线程安全。 */
int ws_client_send_binary(ws_client_t *c, const void *data, size_t len);

/** 是否已连接（握手完成）。 */
int ws_client_is_open(ws_client_t *c);

/** 关闭并释放。阻塞至 poll 线程退出。 */
void ws_client_close(ws_client_t *c);

#ifdef __cplusplus
}
#endif

#endif /* MYAPP_WS_CLIENT_H */

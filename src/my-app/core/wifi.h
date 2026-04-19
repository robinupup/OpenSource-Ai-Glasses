/**
 * my-app core - WiFi 连接模块
 *
 * 通过调用 wpa_supplicant / wpa_cli / udhcpc / dhclient 等工具完成连接。
 *
 * 事件：
 *   "wifi/connecting"   payload: wifi_status_t
 *   "wifi/connected"    payload: wifi_status_t
 *   "wifi/disconnected" payload: wifi_status_t
 *   "wifi/failed"       payload: wifi_status_t
 */

#ifndef MYAPP_WIFI_H
#define MYAPP_WIFI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_STATE_IDLE         = 0,
    WIFI_STATE_CONNECTING   = 1,
    WIFI_STATE_CONNECTED    = 2,
    WIFI_STATE_DISCONNECTED = 3,
    WIFI_STATE_FAILED       = 4,
} wifi_state_t;

typedef struct {
    int  state;            /* wifi_state_t */
    char ssid[64];
    char ip[32];
    int  signal_dbm;
} wifi_status_t;

/** 初始化（启动监控线程） */
int  wifi_init(const char *iface /* 默认 NULL -> wlan0 */);

/** 发起连接。非阻塞，结果通过事件总线通知。 */
int  wifi_connect(const char *ssid, const char *password);

/** 同步等待连接完成。timeout_ms<0 表示无限等待。 */
int  wifi_wait_connected(int timeout_ms);

/** 断开 */
int  wifi_disconnect(void);

/** 读取当前状态 */
int  wifi_get_status(wifi_status_t *out);

/** 关闭模块 */
void wifi_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

#include "app_vlm.h"
#include "app_common.h"

#include "../core/net/ws_client.h"
#include "../core/net/base64.h"
#include "../core/mic_capture.h"
#include "../core/config.h"
#include "../third_party/cJSON/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

typedef enum {
    ST_IDLE = 0,
    ST_REC,
    ST_WAIT,
    ST_RESULT,
} state_t;

static char          g_host[64] = "192.168.1.100";
static int           g_port     = 8002;
static ws_client_t  *g_ws       = NULL;
static mic_pump_t   *g_mic      = NULL;
static state_t       g_state    = ST_IDLE;
static pthread_mutex_t g_lock   = PTHREAD_MUTEX_INITIALIZER;

static void stop_mic(void);

static void set_status(const char *s) {
    app_ui_set_text(app_common_get_ui()->status_label, s);
}

static void on_pcm(const uint8_t *data, size_t len, void *ud) {
    (void)ud;
    if (g_ws && ws_client_is_open(g_ws)) ws_client_send_binary(g_ws, data, len);
}

static void send_uuid(void) {
    char uu[64]; app_gen_uuid(uu, sizeof(uu));
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"type\":\"uuid\",\"data\":\"%s\"}", uu);
    ws_client_send_text(g_ws, msg);
}

static void send_image_from_file(const char *path) {
    char *b64 = base64_encode_file(path);
    if (!b64) { set_status("✕ 图片编码失败"); return; }
    size_t need = strlen(b64) + 64;
    char *msg = (char *)malloc(need);
    if (!msg) { free(b64); return; }
    int n = snprintf(msg, need, "{\"type\":\"image\",\"data\":\"%s\"}", b64);
    (void)n;
    ws_client_send_text(g_ws, msg);
    free(msg); free(b64);
}

static void handle_json(const char *text, size_t len) {
    cJSON *root = cJSON_ParseWithLength(text, len);
    if (!root) return;
    const cJSON *jt = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(jt)) { cJSON_Delete(root); return; }
    const char *type = jt->valuestring;
    const app_ui_t *ui = app_common_get_ui();

    if (strcmp(type, "asr") == 0) {
        const cJSON *jtxt = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(jtxt)) {
            /* 新一轮识别到达：先抹掉上一轮的正文和缩略图 */
            app_ui_clear_text(ui->content_label);
            app_ui_clear_crop();
            app_ui_show_asr(jtxt->valuestring);
        }
    } else if (strcmp(type, "content") == 0) {
        const cJSON *jd = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (cJSON_IsString(jd)) {
            /* 结果返回：隐藏 ASR，只留答案 */
            app_ui_hide_asr();
            app_ui_append_text(ui->content_label, jd->valuestring);
        }
    } else if (strcmp(type, "image_crop") == 0) {
        const cJSON *jd = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (cJSON_IsObject(jd)) {
            const cJSON *jtxt = cJSON_GetObjectItemCaseSensitive(jd, "text");
            const cJSON *jimg = cJSON_GetObjectItemCaseSensitive(jd, "image_b64");
            app_ui_hide_asr();
            if (cJSON_IsString(jtxt)) {
                app_ui_append_text(ui->content_label, "\n");
                app_ui_append_text(ui->content_label, jtxt->valuestring);
            }
            if (cJSON_IsString(jimg)) app_ui_show_crop_b64_jpeg(jimg->valuestring);
        }
    } else if (strcmp(type, "done") == 0) {
        /* 用户仍在录音（ST_REC）时不要改状态，避免按键被当作新一轮又去 open PCM */
        pthread_mutex_lock(&g_lock);
        if (g_state == ST_WAIT) g_state = ST_IDLE;
        state_t after = g_state;
        pthread_mutex_unlock(&g_lock);
        if (after != ST_REC) set_status("电源键 开/关 收音   触摸板2 退出");
    } else if (strcmp(type, "intent") == 0) {
        /* 仅服务端通告，忽略 */
    } else if (strcmp(type, "error") == 0) {
        const cJSON *jd = cJSON_GetObjectItemCaseSensitive(root, "data");
        char buf[160];
        snprintf(buf, sizeof(buf), "✕ 错误: %s",
                 cJSON_IsString(jd) ? jd->valuestring : "?");
        set_status(buf);
    }
    cJSON_Delete(root);
}

static void on_ws(ws_event_t ev, ws_msg_type_t type, const void *data,
                  size_t len, void *ud) {
    (void)ud;
    if (ev == WS_EV_OPEN) {
        set_status("电源键 开/关 收音   触摸板2 退出");
    } else if (ev == WS_EV_MSG) {
        if (type == WS_MSG_TEXT) handle_json((const char *)data, len);
    } else if (ev == WS_EV_CLOSE) {
        pthread_mutex_lock(&g_lock);
        g_state = ST_IDLE;
        pthread_mutex_unlock(&g_lock);
        set_status("○ 连接已断开(按键将自动重连)");
    } else if (ev == WS_EV_ERROR) {
        set_status("✕ 连接错误");
    }
}

static int ensure_ws_open(void) {
    if (g_ws && ws_client_is_open(g_ws)) return 1;
    if (g_ws) { ws_client_close(g_ws); g_ws = NULL; }
    char url[256];
    myapp_config_build_url(url, sizeof(url), g_host, g_port, "/scene_words");
    printf("[scene_words] (re)open %s\n", url);
    g_ws = ws_client_open(url);
    if (!g_ws) { set_status("✕ WS 打开失败"); return 0; }
    ws_client_set_on_msg(g_ws, on_ws, NULL);
    for (int i = 0; i < 40 && !ws_client_is_open(g_ws); i++) usleep(50 * 1000);
    return ws_client_is_open(g_ws);
}

void app_vlm_configure(const char *host, int port) {
    if (host && *host) snprintf(g_host, sizeof(g_host), "%s", host);
    if (port > 0) g_port = port;
}

static void start_round(void) {
    const app_ui_t *ui = app_common_get_ui();
    app_ui_hide_asr();
    app_ui_set_text(ui->content_label, "");
    set_status("○ 拍照中…");
    char jpeg_path[256] = {0};
    if (myapp_take_photo(jpeg_path, sizeof(jpeg_path)) != 0) {
        set_status("✕ 拍照失败");
        return;
    }
    if (!ensure_ws_open()) { set_status("✕ WS 未就绪"); return; }

    send_uuid();
    send_image_from_file(jpeg_path);

    if (g_mic) stop_mic(); /* 防御：上一轮 mic 未关时先关干净 */
    mic_params_t p = { .sample_rate = 16000, .channels = 1, .bit_width = 16 };
    g_mic = mic_pump_start(&p, 1280, on_pcm, NULL);
    if (!g_mic) { set_status("✕ 麦克风启动失败"); return; }
    app_common_set_mic_on(1);
    set_status("● 录音中(电源键 关)");
}

static void stop_mic(void) {
    if (g_mic) { mic_pump_stop(g_mic); g_mic = NULL; }
    app_common_set_mic_on(0);
}

void app_vlm_enter(void) {
    pthread_mutex_lock(&g_lock);
    g_state = ST_IDLE;
    pthread_mutex_unlock(&g_lock);
    const app_ui_t *ui = app_common_get_ui();
    app_ui_hide_asr();
    app_ui_set_text(ui->content_label, "");
    set_status("○ 连接中…");
    ensure_ws_open();
}

void app_vlm_exit(void) {
    stop_mic();
    if (g_ws) {
        /* 对齐 scene_words 协议：优雅结束会话再断开 WS */
        if (ws_client_is_open(g_ws)) {
            ws_client_send_text(g_ws, "{\"type\":\"end\"}");
        }
        ws_client_close(g_ws);
        g_ws = NULL;
    }
    pthread_mutex_lock(&g_lock);
    g_state = ST_IDLE;
    pthread_mutex_unlock(&g_lock);
}

void app_vlm_on_confirm(void) {
    pthread_mutex_lock(&g_lock);
    state_t s = g_state;
    pthread_mutex_unlock(&g_lock);

    if (s != ST_REC) {
        start_round();
        pthread_mutex_lock(&g_lock);
        g_state = ST_REC;
        pthread_mutex_unlock(&g_lock);
    } else {
        /* ST_REC：停麦即可，服务端 server_vad 会自动检测句尾并触发业务；
         * scene_words 协议中没有 mic 控制指令，不要再发多余报文。 */
        stop_mic();
        pthread_mutex_lock(&g_lock);
        g_state = ST_WAIT;
        pthread_mutex_unlock(&g_lock);
        set_status("… 识别中(电源键再按开始新一轮)");
    }
}

/**
 * app_vlm.c — 场景单词（VAD 自动触发版）
 *
 * 对齐 x_engine/src/apps/lumina/scene_words/serve.py：
 *   上行：二进制 PCM(16k/1ch/16bit) | {type:image|uuid|text|end}
 *   下行：{type:asr|intent|content|image_crop|done|error}
 *
 * 交互变化：
 *   - 进入页面即启动麦 + 本地 Silero VAD 监听；不再靠按键开始一轮。
 *   - VAD SPEECH_START → 后台线程拍一帧 JPEG → 发 {type:uuid}/{type:image}；
 *     同帧开始把 PCM 作为二进制推给服务端；服务端 server_vad 自行检测
 *     句尾并触发业务（ASR → 场景单词 handler）。
 *   - 服务端 {type:done} → 回到 ARMED，vad_reset，等下一次开口。
 *   - 电源键（on_confirm）用来 ARMED ↔ PAUSED 切换，方便用户临时关麦。
 *   - 触摸板 2 / 退出 app 时发 {type:end} 并关 WS。
 */
#include "app_vlm.h"
#include "app_common.h"

#include "../core/net/ws_client.h"
#include "../core/net/base64.h"
#include "../core/mic_capture.h"
#include "../core/vad.h"
#include "../core/config.h"
#include "../third_party/cJSON/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

/* ---- 常量 -------------------------------------------------------------- */

#define MIC_SAMPLE_RATE   16000
#define MIC_CHUNK_BYTES   1280          /* 40 ms */
#define VAD_MODEL_PATH    "/oem/etc/silero_vad.ort"

/* ---- 状态机 ------------------------------------------------------------ */
/* IDLE      : 未进入 / 已退出
 * ARMED     : 麦开 + VAD 监听中，未开口
 * STREAMING : VAD 已触发，正在往服务端推图 + 音频，等 server 的 done
 * PAUSED    : 用户按电源键暂停，麦关闭；再按一次回 ARMED
 */
typedef enum {
    VLM_IDLE = 0,
    VLM_ARMED,
    VLM_STREAMING,
    VLM_PAUSED,
} vlm_mode_t;

/* ---- 全局 -------------------------------------------------------------- */

static char              g_host[64] = "192.168.1.100";
static int               g_port     = 8002;

static ws_client_t      *g_ws       = NULL;
static mic_pump_t       *g_mic      = NULL;
static vad_t            *g_vad      = NULL;
static pthread_mutex_t   g_lock     = PTHREAD_MUTEX_INITIALIZER;
static vlm_mode_t        g_mode     = VLM_IDLE;

/* 本轮拍照线程忙标志（同一时刻只允许一个） */
static volatile int      g_snap_busy = 0;

/* ---- UI helpers -------------------------------------------------------- */

static void set_status(const char *s) {
    app_ui_set_text(app_common_get_ui()->status_label, s);
}

/* ---- mode 读写 --------------------------------------------------------- */

static vlm_mode_t mode_get(void) {
    pthread_mutex_lock(&g_lock);
    vlm_mode_t m = g_mode;
    pthread_mutex_unlock(&g_lock);
    return m;
}

static void mode_set(vlm_mode_t m) {
    pthread_mutex_lock(&g_lock);
    g_mode = m;
    pthread_mutex_unlock(&g_lock);
}

/* ---- WS 协议帮手 ------------------------------------------------------- */

static void ws_send_uuid_image_thread(const char *uuid, const char *b64_image) {
    if (!g_ws || !ws_client_is_open(g_ws)) return;
    char umsg[128];
    snprintf(umsg, sizeof(umsg), "{\"type\":\"uuid\",\"data\":\"%s\"}", uuid);
    ws_client_send_text(g_ws, umsg);

    size_t need = strlen(b64_image) + 64;
    char *imsg = (char *)malloc(need);
    if (!imsg) return;
    snprintf(imsg, need, "{\"type\":\"image\",\"data\":\"%s\"}", b64_image);
    ws_client_send_text(g_ws, imsg);
    free(imsg);
}

/* ---- 后台拍照线程 ------------------------------------------------------ */
/* VAD 触发后立刻启动：拍 JPEG → base64 → 发 uuid/image。耗时 200~400ms，
 * 不会阻塞 mic_pump / VAD 回调；音频流与拍照并行推进。 */
static void *snapshot_thread(void *arg) {
    (void)arg;

    char jpeg_path[128] = {0};
    if (myapp_take_photo(jpeg_path, sizeof(jpeg_path)) != 0) {
        printf("[scene_words] take_photo failed\n");
        set_status("✕ 拍照失败");
        g_snap_busy = 0;
        return NULL;
    }

    char *b64 = base64_encode_file(jpeg_path);
    /* 落盘只是中转，编码完就可删；失败不致命 */
    unlink(jpeg_path);

    if (!b64) {
        printf("[scene_words] base64 encode failed\n");
        set_status("✕ 图片编码失败");
        g_snap_busy = 0;
        return NULL;
    }

    char uu[64];
    app_gen_uuid(uu, sizeof(uu));
    ws_send_uuid_image_thread(uu, b64);
    printf("[scene_words] uuid+image sent (uuid=%s, b64=%zuB)\n", uu, strlen(b64));

    free(b64);
    g_snap_busy = 0;
    return NULL;
}

static int kick_snapshot(void) {
    if (g_snap_busy) return 0;
    g_snap_busy = 1;
    pthread_t tid;
    if (pthread_create(&tid, NULL, snapshot_thread, NULL) != 0) {
        g_snap_busy = 0;
        return 0;
    }
    pthread_detach(tid);
    return 1;
}

/* ---- VAD 事件（运行在 mic_pump 线程） ---------------------------------- */

static void on_vad_event(vad_event_t ev,
                         const int16_t *pcm, size_t samples,
                         float prob, void *ud) {
    (void)ud; (void)prob;

    vlm_mode_t m = mode_get();

    if (ev == VAD_EV_SPEECH_START) {
        /* 只在 ARMED 响应；STREAMING / PAUSED 下忽略（本轮还没结束或已暂停） */
        if (m != VLM_ARMED) return;

        /* 先切状态，拒绝重复入场 */
        mode_set(VLM_STREAMING);
        printf("[scene_words] >>> SPEECH_START → snap + stream\n");
        set_status("● 录音中…");

        /* 起一个后台拍照线程，拍完异步发 uuid/image；这条 callback 线程
         * 继续把当前这段开口 PCM 透传给服务端，不被拍照阻塞。 */
        kick_snapshot();

        /* SPEECH_START 自带的这段 PCM 也是用户说话首帧，顺手透传一次。
         * Silero VAD 的 SPEECH_START 通常 pcm/samples 为 0（事件型），
         * 不为 0 再发。 */
        if (g_ws && ws_client_is_open(g_ws) && pcm && samples > 0) {
            ws_client_send_binary(g_ws, pcm, samples * sizeof(int16_t));
        }

    } else if (ev == VAD_EV_SPEECH_CHUNK) {
        /* 只在 STREAMING 下透传 PCM；ARMED 下的 CHUNK 是无关噪声，忽略。 */
        if (m != VLM_STREAMING) return;
        if (g_ws && ws_client_is_open(g_ws) && pcm && samples > 0) {
            ws_client_send_binary(g_ws, pcm, samples * sizeof(int16_t));
        }

    } else if (ev == VAD_EV_SPEECH_END) {
        /* 客户端 VAD 判定的结束——不做 audio_end（scene_words 协议没有此指令），
         * 让服务端 server_vad 基于自己的 silence_duration_ms 自行收口，
         * 随后发 asr/content/image_crop，最后 done。
         * 这里只打个日志，不改状态。 */
        printf("[scene_words] <<< SPEECH_END (wait server done)\n");
    }
}

/* ---- mic_pump 回调：直接喂 VAD ----------------------------------------- */

static void on_pcm(const uint8_t *data, size_t len, void *ud) {
    (void)ud;
    vlm_mode_t m = mode_get();
    if (m != VLM_ARMED && m != VLM_STREAMING) return;
    if (!g_vad) return;
    vad_feed(g_vad, (const int16_t *)data, len / sizeof(int16_t));
}

/* ---- 服务端 JSON ------------------------------------------------------- */

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
            /* 新一轮 asr 首包到达：抹掉上一轮的正文与缩图 */
            app_ui_clear_text(ui->content_label);
            app_ui_clear_crop();
            app_ui_show_asr(jtxt->valuestring);
        }
    } else if (strcmp(type, "content") == 0) {
        const cJSON *jd = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (cJSON_IsString(jd)) {
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
        /* 一轮业务结束：回到 ARMED，VAD 复位等下一次开口 */
        vlm_mode_t m = mode_get();
        if (m == VLM_STREAMING) {
            if (g_vad) vad_reset(g_vad);
            mode_set(VLM_ARMED);
            set_status("● 监听中（开口自动触发；电源键 暂停）");
        }
    } else if (strcmp(type, "intent") == 0) {
        /* 固定为场景单词，无需处理 */
    } else if (strcmp(type, "error") == 0) {
        const cJSON *jd = cJSON_GetObjectItemCaseSensitive(root, "data");
        char buf[160];
        snprintf(buf, sizeof(buf), "✕ 错误: %s",
                 cJSON_IsString(jd) ? jd->valuestring : "?");
        set_status(buf);
        /* 出错也尽量复位监听，避免卡死 */
        vlm_mode_t m = mode_get();
        if (m == VLM_STREAMING) {
            if (g_vad) vad_reset(g_vad);
            mode_set(VLM_ARMED);
        }
    }
    cJSON_Delete(root);
}

static void on_ws(ws_event_t ev, ws_msg_type_t type, const void *data,
                  size_t len, void *ud) {
    (void)ud;
    if (ev == WS_EV_OPEN) {
        set_status("● 监听中（开口自动触发；电源键 暂停）");
    } else if (ev == WS_EV_MSG) {
        if (type == WS_MSG_TEXT) handle_json((const char *)data, len);
    } else if (ev == WS_EV_CLOSE) {
        mode_set(VLM_IDLE);
        set_status("○ 连接已断开(按键将自动重连)");
    } else if (ev == WS_EV_ERROR) {
        set_status("✕ 连接错误");
    }
}

/* ---- bring-up / tear-down --------------------------------------------- */

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

static int ensure_vad(void) {
    if (g_vad) return 1;
    vad_config_t vc = {
        .model_path       = VAD_MODEL_PATH,
        .speech_threshold = 0.5f,
        .min_speech_ms    = 250,
        .min_silence_ms   = 600,
        .on_event         = on_vad_event,
        .user_data        = NULL,
    };
    g_vad = vad_create(&vc);
    if (!g_vad) {
        set_status("✕ VAD 模型加载失败 (" VAD_MODEL_PATH ")");
        return 0;
    }
    return 1;
}

static void start_mic(void) {
    if (g_mic) return;
    mic_params_t p = { .sample_rate = MIC_SAMPLE_RATE, .channels = 1, .bit_width = 16 };
    g_mic = mic_pump_start(&p, MIC_CHUNK_BYTES, on_pcm, NULL);
    if (!g_mic) { set_status("✕ 麦克风启动失败"); return; }
    app_common_set_mic_on(1);
}

static void stop_mic(void) {
    if (g_mic) { mic_pump_stop(g_mic); g_mic = NULL; }
    app_common_set_mic_on(0);
    if (g_vad) vad_force_end(g_vad);
}

/* ---- public API -------------------------------------------------------- */

void app_vlm_configure(const char *host, int port) {
    if (host && *host) snprintf(g_host, sizeof(g_host), "%s", host);
    if (port > 0) g_port = port;
}

void app_vlm_enter(void) {
    const app_ui_t *ui = app_common_get_ui();
    app_ui_hide_asr();
    app_ui_set_text(ui->content_label, "");
    app_ui_clear_crop();

    mode_set(VLM_IDLE);
    set_status("○ 连接中…");

    if (!ensure_ws_open()) return;
    if (!ensure_vad())     return;
    vad_reset(g_vad);
    start_mic();
    mode_set(VLM_ARMED);
    set_status("● 监听中（开口自动触发；电源键 暂停）");
}

void app_vlm_exit(void) {
    stop_mic();
    /* 优雅结束会话：服务端支持 {type:end} 结束 WS */
    if (g_ws) {
        if (ws_client_is_open(g_ws)) {
            ws_client_send_text(g_ws, "{\"type\":\"end\"}");
        }
        ws_client_close(g_ws);
        g_ws = NULL;
    }
    if (g_vad) { vad_destroy(g_vad); g_vad = NULL; }
    mode_set(VLM_IDLE);
}

void app_vlm_on_confirm(void) {
    vlm_mode_t m = mode_get();

    if (m == VLM_ARMED) {
        /* ARMED → PAUSED：关麦，暂停监听 */
        stop_mic();
        mode_set(VLM_PAUSED);
        set_status("⏸ 已暂停（电源键再按继续监听）");
    } else if (m == VLM_PAUSED) {
        /* PAUSED → ARMED：重开麦继续监听 */
        if (!ensure_ws_open()) { set_status("✕ WS 未就绪"); return; }
        if (!ensure_vad())     return;
        vad_reset(g_vad);
        start_mic();
        mode_set(VLM_ARMED);
        set_status("● 监听中（开口自动触发；电源键 暂停）");
    } else if (m == VLM_STREAMING) {
        /* 业务进行中强行收尾：停麦让服务端 server_vad 靠静音段自己收口。
         * 不改 state，等服务端 done 后自然回到 ARMED（由 handle_json 切回）。 */
        if (g_vad) vad_force_end(g_vad);
        set_status("… 识别中（稍候）");
    } else {
        /* IDLE：相当于 re-enter */
        app_vlm_enter();
    }
}

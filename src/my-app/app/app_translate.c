#include "app_translate.h"
#include "app_common.h"

#include "../core/net/ws_client.h"
#include "../core/mic_capture.h"
#include "../core/tts_player.h"
#include "../core/config.h"
#include "../third_party/cJSON/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

/* immersive_english 是连续翻译：没有一次性"结果"态。只有
 *   IDLE（未录音）/ REC（录音中）/ WAIT（已关麦等待服务端收尾）。*/
typedef enum {
    ST_IDLE = 0,
    ST_REC,
    ST_WAIT,
} state_t;

static char          g_host[64] = "192.168.1.100";
static int           g_port     = 8004;
static ws_client_t  *g_ws       = NULL;
static mic_pump_t   *g_mic      = NULL;
static state_t       g_state    = ST_IDLE;
static pthread_mutex_t g_lock   = PTHREAD_MUTEX_INITIALIZER;
/* 当前正在播的 TTS 段 seq（由 tts_start 设置，tts_end/tts_stop 清零）。
 * 用于判定 binary frame 是否属于"当前仍然有效"的段——理论上服务端自己保证，
 * 这里再做一层防御：若 cur_seq==0 则忽略 binary。 */
static volatile int  g_cur_tts_seq = 0;

static void set_status(const char *s) {
    const app_ui_t *ui = app_common_get_ui();
    app_ui_set_text(ui->status_label, s);
}

static void on_pcm(const uint8_t *data, size_t len, void *ud) {
    (void)ud;
    if (g_ws && ws_client_is_open(g_ws)) {
        ws_client_send_binary(g_ws, data, len);
    }
}

static void handle_json(const char *text, size_t len) {
    if (!text || len == 0) return;
    cJSON *root = cJSON_ParseWithLength(text, len);
    if (!root) return;
    const cJSON *jt = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(jt)) { cJSON_Delete(root); return; }
    const char *type = jt->valuestring;
    const app_ui_t *ui = app_common_get_ui();

    if (strcmp(type, "asr") == 0) {
        const cJSON *jtxt = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(jtxt)) {
            app_ui_show_asr(jtxt->valuestring);
            app_ui_clear_text(ui->content_label);
        }
    } else if (strcmp(type, "content") == 0) {
        const cJSON *jd = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (cJSON_IsString(jd)) {
            /* 翻译结果返回：隐藏 ASR，只留译文 */
            app_ui_hide_asr();
            app_ui_append_text(ui->content_label, jd->valuestring);
        }
    } else if (strcmp(type, "tts_start") == 0) {
        /* 服务端告知新一段 PCM 即将到来。参数：sample_rate / channels / bits / seq */
        const cJSON *jsr   = cJSON_GetObjectItemCaseSensitive(root, "sample_rate");
        const cJSON *jch   = cJSON_GetObjectItemCaseSensitive(root, "channels");
        const cJSON *jbits = cJSON_GetObjectItemCaseSensitive(root, "bits");
        const cJSON *jseq  = cJSON_GetObjectItemCaseSensitive(root, "seq");
        /* fallback 与 serve.py 的 TTS_SAMPLE_RATE/TTS_FORMAT 默认值对齐（16k/pcm） */
        int sr   = cJSON_IsNumber(jsr)   ? jsr->valueint   : 16000;
        int ch   = cJSON_IsNumber(jch)   ? jch->valueint   : 1;
        int bits = cJSON_IsNumber(jbits) ? jbits->valueint : 16;
        int seq  = cJSON_IsNumber(jseq)  ? jseq->valueint  : 0;
        g_cur_tts_seq = seq;
        tts_player_begin(sr, ch, bits);
    } else if (strcmp(type, "tts_end") == 0) {
        /* 本段合成完毕：让播放器把剩余队列自然播完，然后关设备。 */
        g_cur_tts_seq = 0;
        tts_player_end();
    } else if (strcmp(type, "tts_stop") == 0) {
        /* 本段被打断：立刻清空播放缓冲（硬件 + 软件队列）。 */
        g_cur_tts_seq = 0;
        tts_player_stop();
    } else if (strcmp(type, "tts_subtitle") == 0) {
        /* 字级时间戳（enable_subtitle=true 时才下发）。当前眼镜不展示字级
         * 高亮，显式忽略以避免走到兜底"未知 type"的误判路径。 */
    } else if (strcmp(type, "done") == 0 || strcmp(type, "interrupted") == 0) {
        /* immersive_english 是连续翻译：每说完一句 server 都会发 done；
         * 此时用户很可能还在录音。只有当用户已经主动停麦（ST_WAIT），
         * 收到 done 才回 IDLE；ST_REC 时保持 ST_REC，避免下一次按键误当作
         * "新开一轮" 又去 open 已经被占用的 PCM 设备导致 snd_pcm_open 死锁。*/
        pthread_mutex_lock(&g_lock);
        if (g_state == ST_WAIT) g_state = ST_IDLE;
        state_t after = g_state;
        pthread_mutex_unlock(&g_lock);
        if (after != ST_REC) set_status("电源键 开/关 收音   触摸板2 退出");
    } else if (strcmp(type, "error") == 0) {
        /* 对齐 serve.py: TTS 内部错误时服务端只下发 error，不一定随带
         * tts_stop/tts_end（serve.py _drain_audio 的 err 分支）。此时若
         * 播放器已开就会泄漏，这里兜底清空，保证下一轮 tts_start 能干净重开。*/
        g_cur_tts_seq = 0;
        tts_player_stop();
        const cJSON *jd = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (cJSON_IsString(jd)) {
            char buf[160];
            snprintf(buf, sizeof(buf), "✕ 错误: %s", jd->valuestring);
            set_status(buf);
        }
    }
    cJSON_Delete(root);
}

static void on_ws(ws_event_t ev, ws_msg_type_t type, const void *data,
                  size_t len, void *ud) {
    (void)ud; (void)type;
    if (ev == WS_EV_OPEN) {
        set_status("电源键 开/关 收音   触摸板2 退出");
    } else if (ev == WS_EV_MSG) {
        if (type == WS_MSG_TEXT) {
            handle_json((const char *)data, len);
        } else if (type == WS_MSG_BINARY) {
            /* 只有在当前段仍有效时才喂给播放器（tts_stop/tts_end 后 seq=0）。*/
            if (g_cur_tts_seq != 0) {
                tts_player_feed(data, len);
            }
        }
    } else if (ev == WS_EV_CLOSE) {
        /* immersive_english 是连续翻译会话，服务端每句 done 后继续 while
         * 循环不会主动断开；走到这里通常是网络/服务端异常。标记状态，下次
         * 按键自动重连（ensure_ws_open 内部会重开）。 */
        g_cur_tts_seq = 0;
        tts_player_stop();
        pthread_mutex_lock(&g_lock);
        g_state = ST_IDLE;
        pthread_mutex_unlock(&g_lock);
        set_status("○ 连接已断开(按键将自动重连)");
    } else if (ev == WS_EV_ERROR) {
        g_cur_tts_seq = 0;
        tts_player_stop();
        set_status("✕ 连接错误");
    }
}

/* 尝试打开 WS；已打开则复用。成功返回 1。 */
static int ensure_ws_open(void) {
    if (g_ws && ws_client_is_open(g_ws)) return 1;
    if (g_ws) { ws_client_close(g_ws); g_ws = NULL; }
    char url[256];
    myapp_config_build_url(url, sizeof(url), g_host, g_port, "/immersive_english");
    printf("[translate] (re)open %s\n", url);
    g_ws = ws_client_open(url);
    if (!g_ws) { set_status("✕ WS 打开失败"); return 0; }
    ws_client_set_on_msg(g_ws, on_ws, NULL);
    /* 等握手完成（最多 2s），期间让 poll 线程去建链 */
    for (int i = 0; i < 40 && !ws_client_is_open(g_ws); i++) usleep(50 * 1000);
    return ws_client_is_open(g_ws);
}

void app_translate_configure(const char *host, int port) {
    if (host && *host) snprintf(g_host, sizeof(g_host), "%s", host);
    if (port > 0) g_port = port;
}

static void stop_mic_only(void) {
    if (g_mic) { mic_pump_stop(g_mic); g_mic = NULL; }
    app_common_set_mic_on(0);
}

/* 关麦时立即掐断 TTS：用户想"停播"和"停录"几乎一定是同一个动作。
 * 这个函数在 on_confirm 的 ST_REC 分支调用，也在 exit 时调用。 */
static void stop_mic_and_tts(void) {
    stop_mic_only();
    g_cur_tts_seq = 0;
    tts_player_stop();
}

void app_translate_enter(void) {
    pthread_mutex_lock(&g_lock);
    g_state = ST_IDLE;
    pthread_mutex_unlock(&g_lock);
    const app_ui_t *ui = app_common_get_ui();
    app_ui_hide_asr();
    app_ui_set_text(ui->content_label, "");
    set_status("○ 连接中…");
    ensure_ws_open();
}

void app_translate_exit(void) {
    stop_mic_and_tts();
    if (g_ws) {
        /* 对齐 serve.py: 优雅结束会话 —— 发 {"type":"end"} 让服务端的
         * audio_receiver 主动退出 while 循环并清理 ASR/TTS 资源，再断 WS。*/
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

void app_translate_on_confirm(void) {
    pthread_mutex_lock(&g_lock);
    state_t s = g_state;
    pthread_mutex_unlock(&g_lock);

    if (s == ST_IDLE || s == ST_WAIT) {
        /* 自动保证 WS 就绪，上次服务端关掉也能无感重开 */
        if (!ensure_ws_open()) { set_status("✕ WS 未就绪，请稍候重试"); return; }
        /* 防御：任何状态不一致（例如收到 done 后旧 mic 未关）都先关干净，
         * 否则第二次 open 同一块 /dev/snd/pcmC0D0c 会在内核里 hang 住。*/
        if (g_mic) stop_mic_only();
        mic_params_t p = { .sample_rate = 16000, .channels = 1, .bit_width = 16 };
        g_mic = mic_pump_start(&p, 1280, on_pcm, NULL);
        if (!g_mic) { set_status("✕ 麦克风启动失败"); return; }
        app_common_set_mic_on(1);
        set_status("● 录音中(电源键 关)");
        pthread_mutex_lock(&g_lock);
        g_state = ST_REC;
        pthread_mutex_unlock(&g_lock);
    } else if (s == ST_REC) {
        /* translate：用户主动"关麦"。按产品约定：这一下同时也要让喇叭闭嘴
         * （当前如果还在播上一句的 TTS，应立即停止）。
         * 不发控制帧，让 VAD 自动收尾。 */
        stop_mic_and_tts();
        pthread_mutex_lock(&g_lock);
        g_state = ST_WAIT;
        pthread_mutex_unlock(&g_lock);
        set_status("… 翻译中(电源键再按开始新一轮)");
    }
}

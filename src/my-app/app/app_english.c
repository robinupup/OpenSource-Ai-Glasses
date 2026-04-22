/**
 * app_english.c — 英语对练（见 app_english.h 头文件注释）
 */
#include "app_english.h"
#include "app_common.h"

#include "../core/net/ws_client.h"
#include "../core/mic_capture.h"
#include "../core/tts_player.h"
#include "../core/vad.h"
#include "../core/config.h"
#include "../third_party/cJSON/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

/* --- constants ---------------------------------------------------------- */

/* english_practice 固定 16 kHz / 16-bit / mono 上行； mic_pump 40ms ≈ 1280 B */
#define MIC_SAMPLE_RATE   16000
#define MIC_CHUNK_BYTES   1280      /* 40 ms */
#define MODEL_PATH        "/oem/etc/silero_vad.ort"

/* Binary prefix bytes (see net/protocol.py). */
#define PREFIX_TTS_AUDIO     0x01
#define PREFIX_CAMERA_FRAME  0x03
#define PREFIX_AUDIO_CHUNK   0x04

/* --- global state ------------------------------------------------------- */

typedef enum { MODE_IDLE = 0, MODE_ACTIVE, MODE_PAUSED } eng_mode_t;

static char             g_host[64] = "192.168.1.100";
static int              g_port     = 8005;
static ws_client_t     *g_ws       = NULL;
static mic_pump_t      *g_mic      = NULL;
static vad_t           *g_vad      = NULL;
static pthread_mutex_t  g_lock     = PTHREAD_MUTEX_INITIALIZER;
static eng_mode_t           g_mode     = MODE_IDLE;

/* Are we currently streaming a user utterance to the cloud?
 * Flipped inside the VAD callback (mic_pump thread) — read from on_ws too. */
static volatile int     g_user_speaking = 0;

/* 每句话的流式上行计数：用来验证"是否真在 32ms 一帧地持续推 WS"，而不是
 * 攒到句末一次性发。SPEECH_START 重置，SPEECH_CHUNK 每次 +1，SPEECH_END
 * 打一行统计。开销仅为一次 clock_gettime + 整型自增，不影响时序。 */
static uint64_t         g_utt_t0_ms     = 0;
static uint32_t         g_utt_chunks    = 0;
static uint64_t         g_utt_bytes     = 0;

static uint64_t now_ms_mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* Is the cloud currently sending TTS (agent speaking)?
 * Used for barge-in: if VAD fires while this is true, send interrupt. */
static volatile int     g_agent_speaking = 0;

/* "Fresh frame per turn" snapshot: on SPEECH_START we kick a worker that
 * captures a JPEG and WS-sends it (0x03) as soon as it's ready. The cloud
 * handler waits up to frame_wait_timeout_s (default 500ms) after commit
 * before falling back, so even a ~300ms encode arrives in time for THIS
 * turn's LLM call. Only one worker in flight at a time. */
static volatile int     g_snap_busy   = 0;

/* 自动重连：与 device_main.py 的 DeviceClient 断线重连对齐。
 * 仅在 MODE_ACTIVE / MODE_PAUSED（用户已进入英语对练页面）时触发，
 * IDLE / app_english_exit 流程不会触发。指数退避 500ms→1s→2s→…→10s。*/
static volatile int     g_reconnect_busy = 0;
static volatile int     g_reconnect_stop = 0;

/* --- helpers ------------------------------------------------------------ */

static void set_status(const char *s) {
    const app_ui_t *ui = app_common_get_ui();
    app_ui_set_text(ui->status_label, s);
}

static void clear_ui(void) {
    const app_ui_t *ui = app_common_get_ui();
    app_ui_hide_asr();
    app_ui_set_text(ui->content_label, "");
}

/* ---- TTS control via JSON ---------------------------------------------- */
/* The English service uses a flat 24 kHz / 16-bit TTS binary stream without
 * tts_start / tts_end wrappers (agent_state=speaking replaces tts_start,
 * agent_state=listening replaces tts_end). Open the player lazily on the
 * first 0x01 frame after a "speaking" notification. */
static volatile int g_tts_open = 0;

static void tts_open_if_needed(void) {
    if (!g_tts_open) {
        tts_player_begin(24000, 1, 16);
        g_tts_open = 1;
    }
}

static void tts_close(void) {
    if (g_tts_open) {
        tts_player_end();
        g_tts_open = 0;
    }
}

static void tts_abort(void) {
    if (g_tts_open) {
        tts_player_stop();
        g_tts_open = 0;
    }
}

/* ---- camera snapshot worker ------------------------------------------- */
/* forward decl: we need to ws-send 0x03 from the worker thread. */
static int ws_send_prefixed(uint8_t prefix, const void *payload, size_t n);

static void *snapshot_thread(void *arg) {
    (void)arg;
    uint64_t t0 = now_ms_mono();
    char path[128];
    int ok = myapp_take_photo(path, sizeof(path));
    if (ok != 0) {
        printf("[english] myapp_take_photo failed\n");
        g_snap_busy = 0;
        return NULL;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) { unlink(path); g_snap_busy = 0; return NULL; }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0 || n > 2 * 1024 * 1024) {
        fclose(fp); unlink(path); g_snap_busy = 0; return NULL;
    }
    uint8_t *buf = malloc(n);
    if (!buf) { fclose(fp); unlink(path); g_snap_busy = 0; return NULL; }
    size_t got = fread(buf, 1, n, fp);
    fclose(fp);
    /* 读完立刻删除磁盘文件，英语对练不需要在本地留存照片 */
    if (unlink(path) != 0) {
        printf("[english] unlink %s: %s\n", path, strerror(errno));
    }
    if ((long)got != n) { free(buf); g_snap_busy = 0; return NULL; }

    /* 直接 WS 推送本轮的 0x03 帧（与 device_main.py 的 _send_camera_frame
     * 对齐：audio_start 已经带 has_frame=true，云端会在 turn commit 时最多
     * 等 frame_wait_timeout_s (默认 500ms) 拿到这一帧，再走 VLM / LLM。 */
    int rc = ws_send_prefixed(PREFIX_CAMERA_FRAME, buf, got);
    uint64_t dt = now_ms_mono() - t0;
    if (rc == 0) {
        printf("[english] camera frame sent: %zu B (capture+send=%llums)\n",
               got, (unsigned long long)dt);
    } else {
        printf("[english] camera frame send failed rc=%d (capture=%llums)\n",
               rc, (unsigned long long)dt);
    }
    free(buf);
    g_snap_busy = 0;
    return NULL;
}

/* 返回 1 表示成功启动一个本轮的抓帧线程；返回 0 表示正忙（还没上一轮的），
 * 本轮就只能按 has_frame=false 上。 */
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

/* ---- binary frame senders --------------------------------------------- */

static int ws_send_prefixed(uint8_t prefix, const void *payload, size_t n) {
    if (!g_ws || !ws_client_is_open(g_ws)) return -1;
    uint8_t *buf = malloc(n + 1);
    if (!buf) return -1;
    buf[0] = prefix;
    memcpy(buf + 1, payload, n);
    int rc = ws_client_send_binary(g_ws, buf, n + 1);
    free(buf);
    return rc;
}

static int ws_send_json(const char *type, cJSON *extra) {
    if (!g_ws || !ws_client_is_open(g_ws)) return -1;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    if (extra) {
        /* merge extra's top-level fields */
        cJSON *it = extra->child;
        while (it) {
            cJSON *next = it->next;
            cJSON_DetachItemViaPointer(extra, it);
            cJSON_AddItemToObject(root, it->string, it);
            it = next;
        }
        cJSON_Delete(extra);
    }
    char *s = cJSON_PrintUnformatted(root);
    int rc = ws_client_send_text(g_ws, s);
    free(s);
    cJSON_Delete(root);
    return rc;
}

/* ---- VAD event handler (runs on mic_pump thread) ---------------------- */

static void on_vad_event(vad_event_t ev,
                         const int16_t *pcm, size_t samples,
                         float prob, void *ud) {
    (void)ud; (void)prob;

    if (ev == VAD_EV_SPEECH_START) {
        /* Barge-in: user talks while agent is talking → cut agent off. */
        if (g_agent_speaking) {
            printf("[english] barge-in: sending interrupt\n");
            ws_send_json("interrupt", NULL);
            tts_abort();
            g_agent_speaking = 0;
        }

        /* 本轮抓帧：VAD 刚触发，立刻启动一个后台线程去拍一张 JPEG，拍完
         * 直接 WS 0x03 推给云端。audio_start 同步发 has_frame=true；云端
         * 在 turn commit 时（audio_end + EOU）最多等 frame_wait_timeout_s
         * 才退路到上一帧，拍照 + 推送通常 150~350ms，完全来得及。
         * 若上一个抓帧线程还没跑完（极少见），退化成无图轮次。 */
        int has_frame = kick_snapshot();

        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "has_frame", has_frame);
        ws_send_json("audio_start", j);

        g_user_speaking = 1;
        g_utt_t0_ms  = now_ms_mono();
        g_utt_chunks = 0;
        g_utt_bytes  = 0;
        printf("[english] >>> SPEECH_START (streaming begin)\n");

    } else if (ev == VAD_EV_SPEECH_CHUNK) {
        if (g_user_speaking && pcm && samples > 0) {
            size_t payload = samples * sizeof(int16_t);
            ws_send_prefixed(PREFIX_AUDIO_CHUNK, pcm, payload);
            g_utt_chunks += 1;
            g_utt_bytes  += payload;
            /* 每 10 个块打一次，用来肉眼看到 "持续推送中" 的节奏，
             * 不会刷爆 log（10 块 ≈ 320ms 一行）*/
            if (g_utt_chunks % 10 == 0) {
                uint64_t dt = now_ms_mono() - g_utt_t0_ms;
                printf("[english] ... streaming chunks=%u span=%llums bytes=%llu\n",
                       g_utt_chunks,
                       (unsigned long long)dt,
                       (unsigned long long)g_utt_bytes);
            }
        }

    } else if (ev == VAD_EV_SPEECH_END) {
        if (g_user_speaking) {
            ws_send_json("audio_end", NULL);
            uint64_t dt = now_ms_mono() - g_utt_t0_ms;
            double avg = g_utt_chunks ? (double)dt / g_utt_chunks : 0.0;
            printf("[english] <<< SPEECH_END chunks=%u span=%llums bytes=%llu avg=%.1fms/chunk %s\n",
                   g_utt_chunks,
                   (unsigned long long)dt,
                   (unsigned long long)g_utt_bytes,
                   avg,
                   (g_utt_chunks >= 2 && avg < 80.0) ? "[STREAMING OK]" : "[NOT streaming?]");
            g_user_speaking = 0;
        }
    }
}

/* ---- mic pump callback (raw PCM from arecord) ------------------------- */

static void on_pcm(const uint8_t *data, size_t len, void *ud) {
    (void)ud;
    if (!g_vad || g_mode != MODE_ACTIVE) return;
    /* mic_pump always feeds even number of bytes (16-bit). */
    vad_feed(g_vad, (const int16_t *)data, len / sizeof(int16_t));
}

/* ---- WS JSON handler -------------------------------------------------- */

static void handle_json(const char *text, size_t len) {
    if (!text || len == 0) return;
    cJSON *root = cJSON_ParseWithLength(text, len);
    if (!root) return;
    const cJSON *jt = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(jt)) { cJSON_Delete(root); return; }
    const char *type = jt->valuestring;
    const app_ui_t *ui = app_common_get_ui();

    if (strcmp(type, "agent_state") == 0) {
        /* ── agent_state ────────────────────────────────────────────────────
         * speaking : LLM 首 token 已产出，TTS 开始推送
         *   → 清空 content_label 准备接收新一轮 agent_text（token 流累加）
         *   → 顶部 asr_label 已有 user_text，保留不动，让用户看到"我说了什么"
         * listening: 一轮结束（或被打断后回到等待）
         *   → content_label 保留完整回答；隐藏 asr_label 清屏备用
         * ─────────────────────────────────────────────────────────────────*/
        const cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (cJSON_IsString(js)) {
            if (strcmp(js->valuestring, "speaking") == 0) {
                g_agent_speaking = 1;
                tts_open_if_needed();
                /* 清空内容区，准备流式拼接 agent_text */
                app_ui_set_text(ui->content_label, "");
                set_status("… 回答中（再说话可插话）");
            } else if (strcmp(js->valuestring, "listening") == 0) {
                g_agent_speaking = 0;
                tts_close();
                /* 隐藏顶部 ASR 字幕，内容区保留完整回答 */
                app_ui_hide_asr();
                set_status("● listening（请开口说英语）");
            }
        }
    } else if (strcmp(type, "interrupt_ack") == 0) {
        /* 打断确认：停 TTS，清空内容区（该轮回答作废） */
        g_agent_speaking = 0;
        tts_abort();
        app_ui_hide_asr();
        app_ui_set_text(ui->content_label, "");
    } else if (strcmp(type, "user_text") == 0) {
        /* ── user_text ──────────────────────────────────────────────────────
         * ASR 最终结果，覆盖顶部 asr_label（取代之前的 partial_text）。
         * 格式：[You] <text>，方便用户在眼镜上区分自己说的和 agent 回答。
         * ─────────────────────────────────────────────────────────────────*/
        const cJSON *jtxt = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(jtxt) && jtxt->valuestring[0]) {
            char asr_line[256];
            snprintf(asr_line, sizeof(asr_line), "[You] %s", jtxt->valuestring);
            app_ui_show_asr(asr_line);
        }
    } else if (strcmp(type, "agent_text") == 0) {
        /* ── agent_text ─────────────────────────────────────────────────────
         * LLM token 流，按到达顺序追加到 content_label。
         * agent_state=speaking 时已清空过一次，这里只追加。
         * ─────────────────────────────────────────────────────────────────*/
        const cJSON *jtxt = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(jtxt) && jtxt->valuestring[0]) {
            app_ui_append_text(ui->content_label, jtxt->valuestring);
        }
    } else if (strcmp(type, "partial_text") == 0) {
        /* ── partial_text ───────────────────────────────────────────────────
         * ASR 增量识别（非最终），每次覆盖顶部 asr_label（不是追加）。
         * 格式：[…] <text>，让用户知道还在识别中。
         * ─────────────────────────────────────────────────────────────────*/
        const cJSON *jtxt = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(jtxt) && jtxt->valuestring[0]) {
            char partial_line[256];
            snprintf(partial_line, sizeof(partial_line), "[…] %s", jtxt->valuestring);
            app_ui_show_asr(partial_line);
        }
    } else if (strcmp(type, "metrics") == 0) {
        /* 延迟埋点，只写日志不上屏 */
        const cJSON *jt1 = cJSON_GetObjectItemCaseSensitive(root, "llm_ttft_ms");
        const cJSON *jt2 = cJSON_GetObjectItemCaseSensitive(root, "tts_first_frame_ms");
        const cJSON *jt3 = cJSON_GetObjectItemCaseSensitive(root, "e2e_ms");
        printf("[english] metrics ttft=%.0f tts0=%.0f e2e=%.0f\n",
               cJSON_IsNumber(jt1) ? jt1->valuedouble : -1.0,
               cJSON_IsNumber(jt2) ? jt2->valuedouble : -1.0,
               cJSON_IsNumber(jt3) ? jt3->valuedouble : -1.0);
    }
    cJSON_Delete(root);
}

static void handle_binary(const uint8_t *data, size_t len) {
    if (len == 0) return;
    uint8_t prefix = data[0];
    if (prefix == PREFIX_TTS_AUDIO) {
        tts_open_if_needed();
        tts_player_feed(data + 1, len - 1);
    } else {
        /* 0x03 (camera) / 0x04 (mic) are device→cloud only; ignore if echoed. */
    }
}

/* ---- auto-reconnect worker ------------------------------------------- */
/* Runs in its own thread. Never called from the mongoose poll thread
 * (on_ws) directly — ws_client_close joins that thread. We spawn this
 * worker from WS_EV_CLOSE so it lives outside the callback. */
static void on_ws(ws_event_t ev, ws_msg_type_t type,
                  const void *data, size_t len, void *ud);

static void *reconnect_thread(void *arg) {
    (void)arg;
    int backoff_ms = 500;
    const int backoff_cap_ms = 10000;
    while (!g_reconnect_stop) {
        pthread_mutex_lock(&g_lock);
        eng_mode_t m = g_mode;
        pthread_mutex_unlock(&g_lock);
        if (m == MODE_IDLE) break;   /* 用户已退出，不再重连 */

        if (g_ws && ws_client_is_open(g_ws)) break;  /* 已经重连上了 */

        /* 把旧的 client 关掉（poll 线程里已经 CLOSE 了，只是对象没回收） */
        if (g_ws) { ws_client_close(g_ws); g_ws = NULL; }

        char url[256];
        myapp_config_build_url(url, sizeof(url), g_host, g_port, "/");
        printf("[english] reconnect → %s (backoff=%dms)\n", url, backoff_ms);
        char stat[96];
        snprintf(stat, sizeof(stat), "⟳ 重连中…(退避 %dms)", backoff_ms);
        set_status(stat);

        g_ws = ws_client_open(url);
        if (g_ws) {
            ws_client_set_on_msg(g_ws, on_ws, NULL);
            /* 给 2s 完成握手 */
            for (int i = 0; i < 40 && !ws_client_is_open(g_ws); i++)
                usleep(50 * 1000);
            if (ws_client_is_open(g_ws)) {
                printf("[english] reconnect ok\n");
                /* on_ws 的 WS_EV_OPEN 会设回状态文本 */
                break;
            }
            /* 没握上 → 关掉，重试 */
            ws_client_close(g_ws);
            g_ws = NULL;
        }

        /* 指数退避 ×2，上限 10s。分 100ms 片段睡，便于 stop 打断。 */
        int slept = 0;
        while (slept < backoff_ms && !g_reconnect_stop) {
            usleep(100 * 1000);
            slept += 100;
        }
        backoff_ms = backoff_ms * 2;
        if (backoff_ms > backoff_cap_ms) backoff_ms = backoff_cap_ms;
    }
    g_reconnect_busy = 0;
    return NULL;
}

static void kick_reconnect(void) {
    /* 仅在会话活动状态下重连；IDLE 下（用户已离开页面）不做无意义重连。 */
    pthread_mutex_lock(&g_lock);
    eng_mode_t m = g_mode;
    pthread_mutex_unlock(&g_lock);
    if (m == MODE_IDLE) return;
    if (g_reconnect_busy) return;
    g_reconnect_busy = 1;
    g_reconnect_stop = 0;
    pthread_t tid;
    if (pthread_create(&tid, NULL, reconnect_thread, NULL) == 0) {
        pthread_detach(tid);
    } else {
        g_reconnect_busy = 0;
    }
}

static void on_ws(ws_event_t ev, ws_msg_type_t type,
                  const void *data, size_t len, void *ud) {
    (void)ud;
    if (ev == WS_EV_OPEN) {
        set_status("○ 已连接，按电源键开始对练");
    } else if (ev == WS_EV_MSG) {
        if (type == WS_MSG_TEXT)        handle_json((const char *)data, len);
        else if (type == WS_MSG_BINARY) handle_binary((const uint8_t *)data, len);
    } else if (ev == WS_EV_CLOSE) {
        g_user_speaking  = 0;
        g_agent_speaking = 0;
        tts_abort();
        set_status("⟳ 连接已断开，正在自动重连…");
        kick_reconnect();
    } else if (ev == WS_EV_ERROR) {
        tts_abort();
        set_status("✕ 连接错误，正在重试…");
        /* mongoose 在 ERROR 之后通常紧跟 CLOSE，这里也 kick 一把保底 */
        kick_reconnect();
    }
}

/* ---- bring-up / tear-down --------------------------------------------- */

static int ensure_ws_open(void) {
    if (g_ws && ws_client_is_open(g_ws)) return 1;
    if (g_ws) { ws_client_close(g_ws); g_ws = NULL; }
    char url[256];
    myapp_config_build_url(url, sizeof(url), g_host, g_port, "/");
    printf("[english] (re)open %s\n", url);
    g_ws = ws_client_open(url);
    if (!g_ws) { set_status("✕ WS 打开失败"); return 0; }
    ws_client_set_on_msg(g_ws, on_ws, NULL);
    for (int i = 0; i < 40 && !ws_client_is_open(g_ws); i++) usleep(50 * 1000);
    return ws_client_is_open(g_ws);
}

static int ensure_vad(void) {
    if (g_vad) return 1;
    vad_config_t vc = {
        .model_path       = MODEL_PATH,
        .speech_threshold = 0.5f,
        .min_speech_ms    = 250,
        .min_silence_ms   = 600,
        .on_event         = on_vad_event,
        .user_data        = NULL,
    };
    g_vad = vad_create(&vc);
    if (!g_vad) {
        set_status("✕ VAD 模型加载失败 (检查 /oem/etc/silero_vad.ort)");
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
    if (g_vad) vad_force_end(g_vad);   /* 若当前在说话，补发一次 audio_end */
    g_user_speaking = 0;
}

/* --- public API --------------------------------------------------------- */

void app_english_configure(const char *host, int port) {
    if (host && *host) snprintf(g_host, sizeof(g_host), "%s", host);
    if (port > 0) g_port = port;
}

void app_english_enter(void) {
    pthread_mutex_lock(&g_lock);
    g_mode = MODE_IDLE;
    pthread_mutex_unlock(&g_lock);
    clear_ui();
    set_status("○ 连接中…");
    ensure_ws_open();
    ensure_vad();              /* 预加载 VAD；即便 WS 没开也不白浪费 RAM */
}

void app_english_exit(void) {
    /* 先置 IDLE，让 reconnect_thread 的下一次循环检测到就退出；再设 stop
     * 标志打断正在 usleep 的退避。两步缺一不可。 */
    pthread_mutex_lock(&g_lock);
    g_mode = MODE_IDLE;
    pthread_mutex_unlock(&g_lock);
    g_reconnect_stop = 1;
    /* 等一小会儿让 reconnect_thread 看到标志退出（它是 detached，不 join） */
    for (int i = 0; i < 50 && g_reconnect_busy; i++) usleep(20 * 1000);

    stop_mic();
    tts_abort();
    if (g_ws)  { ws_client_close(g_ws); g_ws = NULL; }
    if (g_vad) { vad_destroy(g_vad);    g_vad = NULL; }
}

void app_english_on_confirm(void) {
    pthread_mutex_lock(&g_lock);
    eng_mode_t m = g_mode;
    pthread_mutex_unlock(&g_lock);

    if (m == MODE_IDLE || m == MODE_PAUSED) {
        if (!ensure_ws_open()) { set_status("✕ WS 未就绪"); return; }
        if (!ensure_vad())     { return; }
        vad_reset(g_vad);
        /* 首轮也在 SPEECH_START 现抓，不再预热——与 device_main.py 对齐：
         * 每一轮都用"本轮触发瞬间"的画面。 */
        start_mic();
        set_status("● listening（请开口说英语）");
        pthread_mutex_lock(&g_lock);
        g_mode = MODE_ACTIVE;
        pthread_mutex_unlock(&g_lock);
    } else { /* MODE_ACTIVE → pause */
        stop_mic();
        tts_abort();
        g_agent_speaking = 0;
        set_status("⏸ 已暂停（再按开始）");
        pthread_mutex_lock(&g_lock);
        g_mode = MODE_PAUSED;
        pthread_mutex_unlock(&g_lock);
    }
}

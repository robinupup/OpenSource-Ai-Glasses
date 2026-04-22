#include "app_homework.h"
#include "app_common.h"
#include "hw_tree.h"

#include "../core/net/ws_client.h"
#include "../core/net/base64.h"
#include "../core/mic_capture.h"
#include "../core/vad.h"
#include "../core/config.h"
#include "../third_party/cJSON/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <errno.h>

/* ================================================================
 * 拍照搜题 · VAD 驱动版（参考英语对练 app_english.c 的 VAD 集成）
 *
 * 状态机：
 *   IDLE    刚进入功能，WS 尚未就绪
 *   LISTEN  mic 开启，VAD 在等用户开口；每次开口 → 自动进入 SPEAK
 *   SPEAK   VAD 已判定开口；已发送 uuid + image，正在把 PCM 透传给服务端
 *   WAIT    VAD 判定闭口；已发送 mic=off，等待服务端 done
 *   PAUSED  用户按电源键暂停；mic 停，VAD 不再触发，页面保留树可浏览/expand
 *
 * VAD 触发时序（与用户要求对齐）：
 *   SPEECH_START → 新 uuid + 后台线程拍照 + 发 image + 开始上行 PCM
 *   SPEECH_CHUNK → 把本帧 PCM 作为二进制帧直发服务端（与原协议对齐，裸 PCM 无前缀）
 *   SPEECH_END   → 发 {"type":"mic","value":"off"}，等服务端流式返回知识树
 *
 * 按键（在本功能内）：
 *   电源键     = on_confirm   → 暂停 / 恢复监听（mute toggle）
 *   T1 单击   = on_nav_down  → 光标下移（DFS 序）
 *   T1 长按   = on_act_expand → 对当前光标节点 expand（保持原逻辑）
 *   T2 双击   = 退出（main.c 处理）
 * ================================================================ */

typedef enum {
    ST_IDLE = 0,
    ST_LISTEN,
    ST_SPEAK,
    ST_WAIT,
    ST_PAUSED,
} state_t;

#define MODEL_PATH       "/oem/etc/silero_vad.ort"
#define MIC_SAMPLE_RATE  16000
#define MIC_CHUNK_BYTES  1280   /* 40ms @16k/16bit mono，与 app_english 对齐 */

static char            g_host[64]       = "192.168.1.100";
static int             g_port           = 8003;
static ws_client_t    *g_ws             = NULL;
static mic_pump_t     *g_mic            = NULL;
static vad_t          *g_vad            = NULL;
static state_t         g_state          = ST_IDLE;
static pthread_mutex_t g_lock           = PTHREAD_MUTEX_INITIALIZER;

/* 树 / 光标 / 本轮 root uuid */
static hw_node_t       *g_tree          = NULL;
static const hw_node_t *g_cursor        = NULL;
static char             g_root_uuid[64] = {0};

/* expand 等待中：此次返回的 children 应挂到 [g_expand_target] */
static const hw_node_t *g_expand_target = NULL;

/* 抓帧后台线程"忙标记"：与 app_english 同款，保证同一轮只有一条拍照线程 */
static volatile int    g_snap_busy      = 0;

/* 保存"当前轮"的 uuid 给拍照线程使用；抓到 JPEG 后会把 uuid+image 一并发给服务端 */
static char            g_snap_uuid[64]  = {0};

/* 渲染缓冲 */
#define TREE_RENDER_BUF  8192
static char g_render_buf[TREE_RENDER_BUF];

/* ================= UI helpers ================= */

static void set_status(const char *s) {
    app_ui_set_text(app_common_get_ui()->status_label, s);
}

static void refresh_tree_display(void) {
    int cursor_line = -1;
    hw_tree_render(g_tree, g_cursor, g_render_buf, sizeof(g_render_buf),
                   &cursor_line);
    app_ui_set_text(app_common_get_ui()->content_label, g_render_buf);
    app_ui_set_tree_cursor(cursor_line);
}

static void reset_tree(void) {
    if (g_tree) { hw_tree_free(g_tree); g_tree = NULL; }
    g_cursor = NULL;
    g_expand_target = NULL;
    g_tree = hw_tree_new_root();
}

static uint64_t now_ms_mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* ================= 拍照后台线程 =================
 * VAD 回调跑在 mic_pump 线程里，不能在回调中阻塞去抓 JPEG（~300ms），
 * 否则下一帧 PCM 就会堆积导致 VAD 漏帧。起一个 detached 线程完成
 * 拍照 + base64 + WS 发送，全程不占 mic 线程。
 *
 * 与 app_english.c::snapshot_thread 同款，只是把 binary 0x03 改成
 * JSON {"type":"image","data":"<b64>"}，匹配 photo_search 的协议。 */
static void *snapshot_thread(void *arg) {
    (void)arg;
    uint64_t t0 = now_ms_mono();
    char jpeg_path[256] = {0};
    if (myapp_take_photo(jpeg_path, sizeof(jpeg_path)) != 0) {
        printf("[homework] snapshot_thread: take_photo 失败\n");
        g_snap_busy = 0;
        return NULL;
    }

    char *b64 = base64_encode_file(jpeg_path);
    /* 读完立即删，眼镜本地不留题目照片 */
    if (jpeg_path[0]) unlink(jpeg_path);
    if (!b64) {
        printf("[homework] snapshot_thread: base64 编码失败\n");
        g_snap_busy = 0;
        return NULL;
    }

    if (g_ws && ws_client_is_open(g_ws)) {
        size_t need = strlen(b64) + 64;
        char *m = (char *)malloc(need);
        if (m) {
            snprintf(m, need, "{\"type\":\"image\",\"data\":\"%s\"}", b64);
            ws_client_send_text(g_ws, m);
            free(m);
            printf("[homework] image 已发送 (capture+send=%llums)\n",
                   (unsigned long long)(now_ms_mono() - t0));
        }
    } else {
        printf("[homework] snapshot_thread: WS 已断，丢弃本轮 image\n");
    }
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

/* ================= VAD 回调（运行在 mic_pump 线程） ================= */

static void on_vad_event(vad_event_t ev,
                         const int16_t *pcm, size_t samples,
                         float prob, void *ud) {
    (void)ud; (void)prob;

    if (ev == VAD_EV_SPEECH_START) {
        pthread_mutex_lock(&g_lock);
        /* 仅 LISTEN 允许启动新一轮：SPEAK/WAIT/PAUSED 全部忽略，
         * 避免同一服务端会话里并发多轮造成 uuid 错乱 / tree 错挂。*/
        if (g_state != ST_LISTEN) {
            pthread_mutex_unlock(&g_lock);
            printf("[homework] VAD SPEECH_START 在非 LISTEN(state=%d) 态被忽略\n",
                   (int)g_state);
            return;
        }
        /* 新一轮：清树、起新 uuid、触发拍照 */
        reset_tree();
        app_gen_uuid(g_root_uuid, sizeof(g_root_uuid));
        snprintf(g_snap_uuid, sizeof(g_snap_uuid), "%s", g_root_uuid);
        g_state = ST_SPEAK;
        pthread_mutex_unlock(&g_lock);

        app_ui_clear_text(app_common_get_ui()->content_label);
        app_ui_hide_asr();
        app_ui_set_tree_cursor(-1);
        set_status("● 录音中（闭口自动搜题）");

        /* 协议要求：uuid 与 image 都要在 mic=off 之前到齐。
         * 先同步发 uuid（几十字节，毫秒级），再异步起线程发 image。 */
        if (g_ws && ws_client_is_open(g_ws)) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "{\"type\":\"uuid\",\"data\":\"%s\"}", g_root_uuid);
            ws_client_send_text(g_ws, msg);
        }
        kick_snapshot();
        printf("[homework] >>> SPEECH_START uuid=%s\n", g_root_uuid);

    } else if (ev == VAD_EV_SPEECH_CHUNK) {
        /* 仅 SPEAK 态下转发 PCM；WAIT/PAUSED 下的残余块丢弃。 */
        pthread_mutex_lock(&g_lock);
        int ok = (g_state == ST_SPEAK);
        pthread_mutex_unlock(&g_lock);
        if (!ok || !pcm || samples == 0) return;
        if (g_ws && ws_client_is_open(g_ws)) {
            /* photo_search 协议：原始 PCM 二进制帧（无前缀字节），
             * 与原 on_pcm 行为一致，服务端直接喂给 ASR。 */
            ws_client_send_binary(g_ws, (const uint8_t *)pcm,
                                  samples * sizeof(int16_t));
        }

    } else if (ev == VAD_EV_SPEECH_END) {
        pthread_mutex_lock(&g_lock);
        int should_commit = (g_state == ST_SPEAK);
        if (should_commit) g_state = ST_WAIT;
        pthread_mutex_unlock(&g_lock);
        if (!should_commit) return;

        if (g_ws && ws_client_is_open(g_ws)) {
            ws_client_send_text(g_ws, "{\"type\":\"mic\",\"value\":\"off\"}");
        }
        set_status("… 搜题中");
        printf("[homework] <<< SPEECH_END → mic=off\n");
    }
}

/* ================= mic pump 回调：把 PCM 喂 VAD ================= */

static void on_pcm(const uint8_t *data, size_t len, void *ud) {
    (void)ud;
    if (!g_vad) return;
    /* mic_pump 保证偶数字节（16-bit） */
    vad_feed(g_vad, (const int16_t *)data, len / sizeof(int16_t));
}

/* ================= WebSocket ================= */

/*
 * 处理服务端 JSON 消息。类型同原版：asr / content / done / error。
 */
static void handle_json(const char *text, size_t len) {
    cJSON *root = cJSON_ParseWithLength(text, len);
    if (!root) return;
    const cJSON *jt = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(jt)) { cJSON_Delete(root); return; }
    const char *type = jt->valuestring;

    if (strcmp(type, "asr") == 0) {
        const cJSON *jtxt = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(jtxt)) {
            pthread_mutex_lock(&g_lock);
            reset_tree();
            pthread_mutex_unlock(&g_lock);
            app_ui_clear_text(app_common_get_ui()->content_label);
            app_ui_set_tree_cursor(-1);
            app_ui_show_asr(jtxt->valuestring);
        }

    } else if (strcmp(type, "content") == 0) {
        /*
         * 三种 content 形态（与 x_engine/photo_search/serve.py 对齐）：
         *   1) Solve（正常）：data 为字符串 —— 完整 JSON 树 "{\"nodes\":[...]}"
         *   2) Solve（回落）：data 为字符串 —— 纯答案文本（build_tree 失败时）
         *   3) Expand：      data 为对象   —— {"children":[...]}
         */
        const cJSON *jd = cJSON_GetObjectItemCaseSensitive(root, "data");
        char *serialized = NULL;
        const char *json_for_load = NULL;
        int is_plain_text = 0;
        if (cJSON_IsString(jd) && jd->valuestring) {
            json_for_load = jd->valuestring;
            const char *p = json_for_load;
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (*p != '{') is_plain_text = 1;
        } else if (cJSON_IsObject(jd) || cJSON_IsArray(jd)) {
            serialized    = cJSON_PrintUnformatted(jd);
            json_for_load = serialized;
        }

        if (is_plain_text) {
            app_ui_hide_asr();
            pthread_mutex_lock(&g_lock);
            reset_tree();
            pthread_mutex_unlock(&g_lock);
            app_ui_set_text(app_common_get_ui()->content_label, json_for_load);
            app_ui_set_tree_cursor(-1);
        } else if (json_for_load) {
            app_ui_hide_asr();
            pthread_mutex_lock(&g_lock);
            hw_node_t *parent = NULL;
            if (g_expand_target) {
                parent = (hw_node_t *)g_expand_target;
                hw_tree_clear_children(parent);
            } else {
                parent = g_tree;
            }
            int before = parent ? parent->child_count : 0;
            int added  = hw_tree_load_json_into(parent, json_for_load);
            if (!g_cursor && g_tree && g_tree->child_count > 0) {
                g_cursor = g_tree->children[0];
            }
            if (g_expand_target && added > 0 && parent->child_count > before) {
                g_cursor = parent->children[before];
                g_expand_target = NULL;
            }
            int parse_failed_for_solve = (added < 0 && !g_expand_target);
            pthread_mutex_unlock(&g_lock);
            if (parse_failed_for_solve) {
                pthread_mutex_lock(&g_lock);
                reset_tree();
                pthread_mutex_unlock(&g_lock);
                app_ui_set_text(app_common_get_ui()->content_label, json_for_load);
                app_ui_set_tree_cursor(-1);
            } else {
                refresh_tree_display();
            }
        }
        if (serialized) free(serialized);

    } else if (strcmp(type, "done") == 0) {
        pthread_mutex_lock(&g_lock);
        if (g_state == ST_WAIT) g_state = ST_LISTEN;
        g_expand_target = NULL;
        state_t after = g_state;
        pthread_mutex_unlock(&g_lock);

        if (after == ST_LISTEN) {
            set_status("○ 请开口说话开始搜题 · T1 光标 · 长按展开");
            /* VAD 重置内部状态，确保下一轮从"静音"基线起判，
             * 否则残留的 hysteresis 可能导致下一轮漏开头。 */
            if (g_vad) vad_reset(g_vad);
        }

    } else if (strcmp(type, "error") == 0) {
        const cJSON *jd = cJSON_GetObjectItemCaseSensitive(root, "data");
        char buf[160];
        snprintf(buf, sizeof(buf), "✕ 错误: %s",
                 cJSON_IsString(jd) ? jd->valuestring : "?");
        set_status(buf);
        pthread_mutex_lock(&g_lock);
        int had_placeholder = (g_expand_target != NULL);
        if (had_placeholder) {
            hw_tree_clear_children((hw_node_t *)g_expand_target);
        }
        g_expand_target = NULL;
        /* 若 solve 轮途中出错，把状态拽回 LISTEN，允许下一次 VAD 再触发 */
        if (g_state == ST_WAIT || g_state == ST_SPEAK) g_state = ST_LISTEN;
        pthread_mutex_unlock(&g_lock);
        if (had_placeholder) refresh_tree_display();
        if (g_vad) vad_reset(g_vad);
    }

    cJSON_Delete(root);
}

static void on_ws(ws_event_t ev, ws_msg_type_t type, const void *data,
                  size_t len, void *ud) {
    (void)ud;
    if (ev == WS_EV_OPEN) {
        pthread_mutex_lock(&g_lock);
        /* 初次连接：若仍在 IDLE，交给 enter/confirm 路径去启 mic；
         * 断线重连回来：保持原 state 不变，让 VAD 继续。 */
        if (g_state == ST_IDLE) g_state = ST_LISTEN;
        pthread_mutex_unlock(&g_lock);
        set_status("○ 请开口说话开始搜题 · T1 光标 · 长按展开");
    } else if (ev == WS_EV_MSG) {
        if (type == WS_MSG_TEXT) handle_json((const char *)data, len);
    } else if (ev == WS_EV_CLOSE) {
        pthread_mutex_lock(&g_lock);
        /* 中断任何进行中的轮次，等待下次 WS 开后重新 LISTEN */
        if (g_state == ST_SPEAK || g_state == ST_WAIT) g_state = ST_LISTEN;
        g_expand_target = NULL;
        pthread_mutex_unlock(&g_lock);
        set_status("○ 连接已断开");
    } else if (ev == WS_EV_ERROR) {
        set_status("✕ 连接错误");
    }
}

static int ensure_ws_open(void) {
    if (g_ws && ws_client_is_open(g_ws)) return 1;
    if (g_ws) { ws_client_close(g_ws); g_ws = NULL; }
    char url[256];
    myapp_config_build_url(url, sizeof(url), g_host, g_port, "/photo_search");
    printf("[homework] (re)open %s\n", url);
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
    mic_params_t p = {
        .sample_rate = MIC_SAMPLE_RATE, .channels = 1, .bit_width = 16,
    };
    g_mic = mic_pump_start(&p, MIC_CHUNK_BYTES, on_pcm, NULL);
    if (!g_mic) { set_status("✕ 麦克风启动失败"); return; }
    app_common_set_mic_on(1);
}

static void stop_mic(void) {
    if (g_mic) { mic_pump_stop(g_mic); g_mic = NULL; }
    app_common_set_mic_on(0);
    /* 若当前在说话态，强制收尾：把 mic=off 补发给服务端，避免 WAIT 永久挂起 */
    if (g_vad) vad_force_end(g_vad);
}

/* ================= 公开接口 ================= */

void app_homework_configure(const char *host, int port) {
    if (host && *host) snprintf(g_host, sizeof(g_host), "%s", host);
    if (port > 0) g_port = port;
}

void app_homework_enter(void) {
    pthread_mutex_lock(&g_lock);
    g_state = ST_IDLE;
    reset_tree();
    pthread_mutex_unlock(&g_lock);
    const app_ui_t *ui = app_common_get_ui();
    app_ui_hide_asr();
    app_ui_set_text(ui->content_label, "开口说话开始搜题（VAD 自动触发）");
    app_ui_set_tree_cursor(-1);
    set_status("○ 连接中…");

    if (!ensure_ws_open()) return;
    if (!ensure_vad())     return;
    vad_reset(g_vad);
    start_mic();
    pthread_mutex_lock(&g_lock);
    g_state = ST_LISTEN;
    pthread_mutex_unlock(&g_lock);
    set_status("○ 请开口说话开始搜题 · T1 光标 · 长按展开");
}

void app_homework_exit(void) {
    stop_mic();
    /* 等 detached 的拍照线程（若在）做完，避免下一次进入时线程残留 */
    for (int i = 0; i < 50 && g_snap_busy; i++) usleep(20 * 1000);
    if (g_ws) { ws_client_close(g_ws); g_ws = NULL; }
    if (g_vad) { vad_destroy(g_vad); g_vad = NULL; }
    pthread_mutex_lock(&g_lock);
    g_state = ST_IDLE;
    if (g_tree) { hw_tree_free(g_tree); g_tree = NULL; }
    g_cursor = NULL;
    g_expand_target = NULL;
    pthread_mutex_unlock(&g_lock);
}

/* 电源键：暂停 / 恢复 VAD 监听（mute toggle）。
 * 暂停时停 mic + 强制 VAD end，状态转 PAUSED；树与光标保留可继续浏览。
 * 恢复时重新开 mic，回到 LISTEN。若恰逢 WAIT 中，优先让其自然回到 LISTEN。 */
void app_homework_on_confirm(void) {
    pthread_mutex_lock(&g_lock);
    state_t s = g_state;
    pthread_mutex_unlock(&g_lock);

    if (s == ST_PAUSED) {
        if (!ensure_ws_open()) { set_status("✕ WS 未就绪"); return; }
        if (!ensure_vad())     return;
        vad_reset(g_vad);
        start_mic();
        pthread_mutex_lock(&g_lock);
        g_state = ST_LISTEN;
        pthread_mutex_unlock(&g_lock);
        set_status("○ 请开口说话开始搜题 · T1 光标 · 长按展开");
    } else {
        /* LISTEN / SPEAK / WAIT / IDLE 一律切到 PAUSED：
         * 若在 SPEAK，vad_force_end 会驱动 SPEECH_END → 发 mic=off 收尾，
         * 但此时再停 mic 不会再有 PCM 丢给服务端。*/
        stop_mic();
        pthread_mutex_lock(&g_lock);
        g_state = ST_PAUSED;
        pthread_mutex_unlock(&g_lock);
        set_status("⏸ 已暂停（电源键=继续 · T1 光标 · 长按展开）");
    }
}

void app_homework_on_nav_down(void) {
    pthread_mutex_lock(&g_lock);
    if (g_tree && g_tree->child_count > 0) {
        g_cursor = hw_tree_next_visible(g_tree, g_cursor);
    }
    pthread_mutex_unlock(&g_lock);
    refresh_tree_display();
}

void app_homework_on_act_expand(void) {
    pthread_mutex_lock(&g_lock);
    const hw_node_t *target = g_cursor;
    int ws_ok = (g_ws && ws_client_is_open(g_ws));
    /* busy 定义：正在本轮 solve 过程中（SPEAK/WAIT）或已有在途 expand。
     * LISTEN / PAUSED / IDLE 下都允许展开。 */
    int busy  = (g_expand_target != NULL)
              || (g_state == ST_SPEAK) || (g_state == ST_WAIT);
    pthread_mutex_unlock(&g_lock);

    if (!target || !target->label) { set_status("✕ 请先开口说话搜题"); return; }
    if (!ws_ok) { set_status("✕ WS 未就绪，无法展开"); return; }
    if (busy)   { set_status("… 请稍候，上一轮尚未结束"); return; }

    pthread_mutex_lock(&g_lock);
    g_expand_target = target;
    hw_node_t *wtarget = (hw_node_t *)target;
    wtarget->is_folded = 0;
    hw_tree_clear_children(wtarget);
    hw_tree_load_json_into(wtarget,
        "{\"children\":[{\"label\":\"查询中...\"}]}");
    pthread_mutex_unlock(&g_lock);
    refresh_tree_display();

    /* expand 协议：{"type":"expand","value":"<label>","data":"<uuid>"}
     * value=节点 label；data=本轮 solve 的 uuid。 */
    size_t need = strlen(target->label) + strlen(g_root_uuid) + 96;
    char *m = (char *)malloc(need);
    if (!m) { set_status("✕ 内存不足"); return; }

    char escaped[512];
    size_t ei = 0;
    for (const char *p = target->label; *p && ei < sizeof(escaped) - 2; p++) {
        if (*p == '\\' || *p == '"') {
            if (ei + 2 >= sizeof(escaped)) break;
            escaped[ei++] = '\\';
        }
        escaped[ei++] = *p;
    }
    escaped[ei] = '\0';

    snprintf(m, need, "{\"type\":\"expand\",\"value\":\"%s\",\"data\":\"%s\"}",
             escaped, g_root_uuid);
    int rc = ws_client_send_text(g_ws, m);
    free(m);

    if (rc != 0) {
        pthread_mutex_lock(&g_lock);
        if (g_expand_target) {
            hw_tree_clear_children((hw_node_t *)g_expand_target);
        }
        g_expand_target = NULL;
        pthread_mutex_unlock(&g_lock);
        refresh_tree_display();
        set_status("✕ 展开请求发送失败");
        return;
    }
    set_status("… 正在展开");
}

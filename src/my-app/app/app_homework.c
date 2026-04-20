#include "app_homework.h"
#include "app_common.h"
#include "hw_tree.h"

#include "../core/net/ws_client.h"
#include "../core/net/base64.h"
#include "../core/mic_capture.h"
#include "../core/config.h"
#include "../third_party/cJSON/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

/* 状态机：
 *   IDLE   空闲；按电源键 → 拍照 + 建 uuid + 开录音 → REC
 *   REC    录音中；再按电源键 → 停录音 + 发 mic=off → WAIT
 *   WAIT   等服务端流式返回知识树；done 之后回到 IDLE（但 tree 保留，允许 expand）
 * expand 操作不改变主状态机；只要 WS 已连接、tree 非空、cursor 有效即可发。
 */
typedef enum { ST_IDLE = 0, ST_REC, ST_WAIT } state_t;

static char            g_host[64]   = "192.168.1.100";
static int             g_port       = 8003;
static ws_client_t    *g_ws         = NULL;
static mic_pump_t     *g_mic        = NULL;
static state_t         g_state      = ST_IDLE;
static pthread_mutex_t g_lock       = PTHREAD_MUTEX_INITIALIZER;

/* 树 / 光标 / 本轮 root uuid */
static hw_node_t       *g_tree      = NULL;
static const hw_node_t *g_cursor    = NULL;
static char             g_root_uuid[64] = {0};

/* expand 等待中：此次返回的 children 应挂到 [g_expand_target]，而非下一个顶层
 * 节点。done 之后清零。 */
static const hw_node_t *g_expand_target = NULL;

/* 渲染缓冲：整棵树展开后的多行字符串（含 UTF-8 中文）。
 * 服务端 build_tree 一题常见 30~80 节点，单节点 label 20~40 字节。
 * 8KB 可容纳大部分情况；溢出时 hw_tree_render 会静默截断。 */
#define TREE_RENDER_BUF  8192
static char g_render_buf[TREE_RENDER_BUF];

/* ================= UI helpers ================= */

static void set_status(const char *s) {
    app_ui_set_text(app_common_get_ui()->status_label, s);
}

static void refresh_tree_display(void) {
    /* 1) 渲染纯文本树；cursor_line 为光标所在 0-based 行号（-1 表示没光标）
     * 2) 把文本写到 content_label
     * 3) 让 UI 层把高亮矩形定位到光标行，并滚动到可视区——字数过多时
     *    content label 被 tree_wrap 裁剪，光标会始终保持在屏幕内。*/
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

/* ================= WebSocket ================= */

static void stop_mic(void);

/* PCM 发送统计：每 1s 打印一次字节数和帧数。
 * 排查"算法服务没有 ASR 返回"时关键——肉眼即可判断麦克风是否在产出、
 * 以及本轮停麦前累计发了多少音频。translate/vlm 也可以按需加同款统计。*/
static void on_pcm(const uint8_t *data, size_t len, void *ud) {
    (void)ud;
    static uint64_t win_start_ms = 0;
    static uint64_t win_bytes = 0;
    static uint32_t win_frames = 0;
    static uint64_t round_bytes = 0;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
    if (win_start_ms == 0) { win_start_ms = now; round_bytes = 0; }

    int ws_up = (g_ws && ws_client_is_open(g_ws));
    if (ws_up) {
        ws_client_send_binary(g_ws, data, len);
        win_bytes   += len;
        win_frames  += 1;
        round_bytes += len;
    }

    if (now - win_start_ms >= 1000) {
        printf("[homework] PCM tx: %u frames, %llu B/s, round_total=%llu B, ws=%d\n",
               win_frames,
               (unsigned long long)win_bytes,
               (unsigned long long)round_bytes,
               ws_up);
        win_start_ms = now;
        win_bytes = 0;
        win_frames = 0;
    }
}

/*
 * 处理服务端 JSON 消息。
 * 可能的 type：
 *   asr       - {text}：用户语音识别（展示到 asr_label）
 *   content   - {data}：data 为 JSON 树字符串（首轮：{"nodes":..}；expand：{"children":..}）
 *   done      - 本轮结束；若在 WAIT 则回 IDLE
 *   error     - 异常
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
            /* 新一轮结果开始：抹掉上一轮的知识树和正文 */
            pthread_mutex_lock(&g_lock);
            reset_tree();
            pthread_mutex_unlock(&g_lock);
            app_ui_clear_text(app_common_get_ui()->content_label);
            app_ui_set_tree_cursor(-1);
            app_ui_show_asr(jtxt->valuestring);
        }

    } else if (strcmp(type, "content") == 0) {
        /*
         * 服务端 x_engine/photo_search/serve.py 的两种 content 形态：
         *   Solve 结束：  data 为字符串 —— 完整 JSON 树 "{\"nodes\":[...]}"
         *   Expand 结束： data 为对象   —— {"children":[...]}   (见 serve.py 第 479 行)
         * 字符串直接喂 loader；对象先 cJSON_PrintUnformatted 再走同一通道。
         */
        const cJSON *jd = cJSON_GetObjectItemCaseSensitive(root, "data");
        char *serialized = NULL;
        const char *json_for_load = NULL;
        if (cJSON_IsString(jd) && jd->valuestring) {
            json_for_load = jd->valuestring;
        } else if (cJSON_IsObject(jd) || cJSON_IsArray(jd)) {
            serialized    = cJSON_PrintUnformatted(jd);
            json_for_load = serialized;
        }
        if (json_for_load) {
            /* 结果返回：隐藏 ASR，只留知识树 */
            app_ui_hide_asr();
            pthread_mutex_lock(&g_lock);
            hw_node_t *parent = NULL;
            if (g_expand_target) {
                parent = (hw_node_t *)g_expand_target;
                /* 清掉占位的"查询中..."节点（也清掉旧的展开结果，防重复） */
                hw_tree_clear_children(parent);
            } else {
                parent = g_tree;
            }
            int before = parent ? parent->child_count : 0;
            int added  = hw_tree_load_json_into(parent, json_for_load);
            /* 若首次构建，把光标落在第一个顶层节点上 */
            if (!g_cursor && g_tree && g_tree->child_count > 0) {
                g_cursor = g_tree->children[0];
            }
            /* expand 成功后，光标跳到新挂的第一个子节点，方便用户继续深入 */
            if (g_expand_target && added > 0 && parent->child_count > before) {
                g_cursor = parent->children[before];
                g_expand_target = NULL;
            }
            pthread_mutex_unlock(&g_lock);
            refresh_tree_display();
        }
        if (serialized) free(serialized);

    } else if (strcmp(type, "done") == 0) {
        pthread_mutex_lock(&g_lock);
        if (g_state == ST_WAIT) g_state = ST_IDLE;
        state_t after = g_state;
        g_expand_target = NULL;   /* expand 也经由 done 收尾 */
        pthread_mutex_unlock(&g_lock);
        if (after != ST_REC) {
            set_status("电源键 收音 · T1 单击 换页 · T1 长按 展开 · T2 退出");
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
        pthread_mutex_unlock(&g_lock);
        if (had_placeholder) refresh_tree_display();
    }

    cJSON_Delete(root);
}

static void on_ws(ws_event_t ev, ws_msg_type_t type, const void *data,
                  size_t len, void *ud) {
    (void)ud;
    if (ev == WS_EV_OPEN) {
        set_status("电源键 收音 · T1 单击 换页 · T1 长按 展开 · T2 退出");
    } else if (ev == WS_EV_MSG) {
        if (type == WS_MSG_TEXT) handle_json((const char *)data, len);
    } else if (ev == WS_EV_CLOSE) {
        pthread_mutex_lock(&g_lock);
        g_state = ST_IDLE;
        g_expand_target = NULL;
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
    myapp_config_build_url(url, sizeof(url), g_host, g_port, "/homework_finding");
    printf("[homework] (re)open %s\n", url);
    g_ws = ws_client_open(url);
    if (!g_ws) { set_status("✕ WS 打开失败"); return 0; }
    ws_client_set_on_msg(g_ws, on_ws, NULL);
    for (int i = 0; i < 40 && !ws_client_is_open(g_ws); i++) usleep(50 * 1000);
    return ws_client_is_open(g_ws);
}

/* ================= 公开接口 ================= */

void app_homework_configure(const char *host, int port) {
    if (host && *host) snprintf(g_host, sizeof(g_host), "%s", host);
    if (port > 0) g_port = port;
}

static void start_round(void) {
    const app_ui_t *ui = app_common_get_ui();
    (void)ui;
    app_ui_hide_asr();
    /* 新一轮：清空上一轮的树 */
    pthread_mutex_lock(&g_lock);
    reset_tree();
    pthread_mutex_unlock(&g_lock);
    app_ui_set_text(ui->content_label, "");
    app_ui_set_tree_cursor(-1);   /* 新一轮：光标还没数据，先把高亮矩形隐掉 */
    set_status("○ 拍照中…");

    char jpeg_path[256] = {0};
    if (myapp_take_photo(jpeg_path, sizeof(jpeg_path)) != 0) {
        set_status("✕ 拍照失败");
        return;
    }
    if (!ensure_ws_open()) { set_status("✕ WS 未就绪"); return; }

    if (g_mic) stop_mic(); /* 防御：上一轮 mic 未关时先关干净 */

    /* uuid - 作为本轮 root 对话 ID，expand 时会复用 */
    app_gen_uuid(g_root_uuid, sizeof(g_root_uuid));
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"type\":\"uuid\",\"data\":\"%s\"}", g_root_uuid);
    ws_client_send_text(g_ws, msg);

    /* image */
    char *b64 = base64_encode_file(jpeg_path);
    if (b64) {
        size_t need = strlen(b64) + 64;
        char *m = (char *)malloc(need);
        if (m) {
            snprintf(m, need, "{\"type\":\"image\",\"data\":\"%s\"}", b64);
            ws_client_send_text(g_ws, m);
            free(m);
        }
        free(b64);
    }

    mic_params_t p = { .sample_rate = 16000, .channels = 1, .bit_width = 16 };
    g_mic = mic_pump_start(&p, 1280, on_pcm, NULL);
    if (!g_mic) { set_status("✕ 麦克风启动失败"); return; }
    app_common_set_mic_on(1);
    set_status("● 录音中(电源键=关)");
}

static void stop_mic(void) {
    if (g_mic) { mic_pump_stop(g_mic); g_mic = NULL; }
    app_common_set_mic_on(0);
}

void app_homework_enter(void) {
    pthread_mutex_lock(&g_lock);
    g_state = ST_IDLE;
    reset_tree();
    pthread_mutex_unlock(&g_lock);
    const app_ui_t *ui = app_common_get_ui();
    app_ui_hide_asr();
    app_ui_set_text(ui->content_label, "按电源键开始拍照 / 收音");
    app_ui_set_tree_cursor(-1);   /* 刚进入功能，没有光标行 → 隐藏高亮矩形 */
    set_status("○ 连接中…");
    ensure_ws_open();
}

void app_homework_exit(void) {
    stop_mic();
    if (g_ws) { ws_client_close(g_ws); g_ws = NULL; }
    pthread_mutex_lock(&g_lock);
    g_state = ST_IDLE;
    if (g_tree) { hw_tree_free(g_tree); g_tree = NULL; }
    g_cursor = NULL;
    g_expand_target = NULL;
    pthread_mutex_unlock(&g_lock);
}

void app_homework_on_confirm(void) {
    pthread_mutex_lock(&g_lock);
    state_t s = g_state;
    pthread_mutex_unlock(&g_lock);

    if (s != ST_REC) {
        /* 先把状态切过去，避免 start_round 期间（拍照+base64+send image 合计 1~2s）
         * 电源键再次按下被误判为"第一次按"导致重入 start_round。 */
        pthread_mutex_lock(&g_lock);
        g_state = ST_REC;
        pthread_mutex_unlock(&g_lock);
        start_round();
    } else {
        stop_mic();
        if (g_ws) ws_client_send_text(g_ws, "{\"type\":\"mic\",\"value\":\"off\"}");
        pthread_mutex_lock(&g_lock);
        g_state = ST_WAIT;
        pthread_mutex_unlock(&g_lock);
        printf("[homework] mic off 已发，进入 WAIT；请观察服务端是否收到足量 PCM 字节\n");
        set_status("… 搜题中");
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
    int busy  = (g_expand_target != NULL) || (g_state == ST_REC) || (g_state == ST_WAIT);
    pthread_mutex_unlock(&g_lock);

    if (!target || !target->label) { set_status("✕ 请先按电源键拍照搜题"); return; }
    if (!ws_ok) { set_status("✕ WS 未就绪，无法展开"); return; }
    if (busy)   { set_status("… 请稍候，上一轮尚未结束"); return; }

    /* 记录期待挂点，响应回来后由 handle_json 挂到这里；同时插入一个
     * "查询中..." 占位子节点，让用户立即看到"展开已发起"。收到 content
     * 时先 clear_children 再 load，占位节点自然被覆盖。 */
    pthread_mutex_lock(&g_lock);
    g_expand_target = target;
    hw_node_t *wtarget = (hw_node_t *)target;
    wtarget->is_folded = 0;   /* 展开前强制解折叠，否则子节点不渲染 */
    hw_tree_clear_children(wtarget);
    hw_tree_load_json_into(wtarget,
        "{\"children\":[{\"label\":\"查询中...\"}]}");
    pthread_mutex_unlock(&g_lock);
    refresh_tree_display();

    /* 构造 expand 消息（与 x_engine/photo_search/serve.py 对齐）：
     *   客户端 → 服务端 : {"type":"expand","value":"<leaf_label>","data":"<uuid>"}
     *   服务端 → 客户端 : {"type":"content","data":{"children":[...]}}  → {"type":"done"}
     * 注意字段名：value=节点 label；data=本轮 solve 的 uuid（用于定位 tree.json）。*/
    size_t need = strlen(target->label) + strlen(g_root_uuid) + 96;
    char *m = (char *)malloc(need);
    if (!m) { set_status("✕ 内存不足"); return; }

    /* 转义 label 中的双引号与反斜杠（防止 JSON 注入） */
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
        /* 发送失败：移除刚挂上去的"查询中..."占位 */
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

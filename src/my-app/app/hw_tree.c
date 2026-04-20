#include "hw_tree.h"

#include "../third_party/cJSON/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 全局自增 id，仅用于调试/定位；不同根之间共享 */
static int g_next_id = 1;

static hw_node_t *new_node(const char *label) {
    hw_node_t *n = (hw_node_t *)calloc(1, sizeof(hw_node_t));
    if (!n) return NULL;
    n->id = g_next_id++;
    if (label) {
        n->label = strdup(label);
        if (!n->label) { free(n); return NULL; }
    }
    return n;
}

static int ensure_children_cap(hw_node_t *p, int need) {
    if (p->child_cap >= need) return 0;
    int cap = p->child_cap ? p->child_cap : 4;
    while (cap < need) cap *= 2;
    hw_node_t **nc = (hw_node_t **)realloc(p->children,
                                           sizeof(hw_node_t *) * (size_t)cap);
    if (!nc) return -1;
    p->children = nc;
    p->child_cap = cap;
    return 0;
}

static int append_child(hw_node_t *parent, hw_node_t *child) {
    if (ensure_children_cap(parent, parent->child_count + 1) != 0) return -1;
    parent->children[parent->child_count++] = child;
    child->parent = parent;
    return 0;
}

hw_node_t *hw_tree_new_root(void) {
    hw_node_t *r = (hw_node_t *)calloc(1, sizeof(hw_node_t));
    return r;   /* id=0, label=NULL, 不参与渲染 */
}

void hw_tree_free(hw_node_t *node) {
    if (!node) return;
    for (int i = 0; i < node->child_count; i++) {
        hw_tree_free(node->children[i]);
    }
    free(node->children);
    free(node->label);
    free(node);
}

int hw_tree_is_empty(const hw_node_t *root) {
    return !root || root->child_count == 0;
}

void hw_tree_clear_children(hw_node_t *parent) {
    if (!parent) return;
    for (int i = 0; i < parent->child_count; i++) {
        hw_tree_free(parent->children[i]);
    }
    parent->child_count = 0;
    /* children 数组 capacity 保留，避免 realloc 抖动 */
}

/* 递归加载 [parent] 下 [arr] 数组里每一项（必须是 object 且含 "label"）。 */
static int load_json_array(hw_node_t *parent, const cJSON *arr) {
    if (!cJSON_IsArray(arr)) return 0;
    int added = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsObject(it)) continue;
        const cJSON *jl = cJSON_GetObjectItemCaseSensitive(it, "label");
        const char *lbl = (cJSON_IsString(jl) && jl->valuestring) ?
                          jl->valuestring : "";
        hw_node_t *n = new_node(lbl);
        if (!n) continue;
        if (append_child(parent, n) != 0) { free(n->label); free(n); continue; }

        const cJSON *jc = cJSON_GetObjectItemCaseSensitive(it, "children");
        if (cJSON_IsArray(jc)) load_json_array(n, jc);
        added++;
    }
    return added;
}

int hw_tree_load_json_into(hw_node_t *parent, const char *json_str) {
    if (!parent || !json_str) return -1;
    cJSON *obj = cJSON_Parse(json_str);
    if (!obj) return -1;
    int n = 0;
    const cJSON *nodes    = cJSON_GetObjectItemCaseSensitive(obj, "nodes");
    const cJSON *children = cJSON_GetObjectItemCaseSensitive(obj, "children");
    if (cJSON_IsArray(nodes))         n = load_json_array(parent, nodes);
    else if (cJSON_IsArray(children)) n = load_json_array(parent, children);
    else                               n = -1;
    cJSON_Delete(obj);
    return n;
}

/* DFS：把 node 自身（若非 root）+ 其未折叠子孙追加进 out。 */
static void dfs_collect(const hw_node_t *node,
                        const hw_node_t *root,
                        const hw_node_t **out, int cap, int *idx) {
    if (node != root) {
        if (*idx < cap) out[(*idx)++] = node;
    }
    if (node->is_folded && node != root) return;  /* 折叠：不递归子节点 */
    for (int i = 0; i < node->child_count; i++) {
        dfs_collect(node->children[i], root, out, cap, idx);
    }
}

int hw_tree_collect_visible(const hw_node_t *root,
                            const hw_node_t **out, int cap) {
    if (!root || !out || cap <= 0) return 0;
    int n = 0;
    dfs_collect(root, root, out, cap, &n);
    return n;
}

const hw_node_t *hw_tree_next_visible(const hw_node_t *root,
                                       const hw_node_t *cursor) {
    if (!root || root->child_count == 0) return NULL;
    const hw_node_t *list[256];
    int n = hw_tree_collect_visible(root, list, 256);
    if (n == 0) return NULL;
    if (!cursor) return list[0];
    for (int i = 0; i < n; i++) {
        if (list[i] == cursor) {
            return list[(i + 1) % n];   /* 循环：末尾后回到首个 */
        }
    }
    return list[0];
}

/* 追加串到 buf（带容量检查，超限静默截断）。 */
static size_t buf_append(char *buf, size_t cap, size_t pos, const char *s) {
    if (!s) return pos;
    size_t ln = strlen(s);
    if (pos + ln >= cap) ln = (cap > pos + 1) ? (cap - 1 - pos) : 0;
    if (ln > 0) memcpy(buf + pos, s, ln);
    return pos + ln;
}

/* 折叠状态下节点内容预览：取首行前 15 个字符（按字节计，保守）。 */
static void folded_preview(const char *label, char *out, size_t outcap) {
    if (!label) { if (outcap) out[0] = '\0'; return; }
    size_t i = 0;
    for (; label[i] && label[i] != '\n' && i < 45; i++) { }
    /* UTF-8 近似：最多 45 字节 ≈ 15 汉字 */
    if (i >= outcap) i = outcap - 1;
    memcpy(out, label, i);
    out[i] = '\0';
}

/* ────────────── 树形连接符（Unicode Box Drawing, U+2500..U+257F）──────────────
 * 与 lumina_edge GlassesUIManager 保持一致，三类前缀按"是否最后一个子节点"
 * 与"祖先是否还有后续兄弟"组合出来：
 *   T_MID  ├─   节点自身：本层还有后续兄弟
 *   T_LAST └─   节点自身：本层最后一个
 *   T_CONT │    祖先层还有后续兄弟时，该层的续线
 *   T_NONE      祖先层已是末尾，该层为空白（3 空格保持对齐）
 * 
 * 字体由 ui_font_math_30（DejaVuSansMono 子集）提供，alibaba_30 的 fallback
 * 链会自动命中。*/
#define T_MID   "├─"
#define T_LAST  "└─"
#define T_CONT  "\xe2\x94\x82  "   /* "│  "（U+2502 + 两空格） */
#define T_NONE  "   "

/* 每渲染完一个节点，行号 +1。光标命中时记录行号写回给调用方。
 * 不再使用 ▶ 光标前缀、也不再用 recolor 颜色标记——整行高亮改由 UI 侧
 * 悬浮矩形实现（见 app_common.c::app_ui_set_tree_cursor）。*/
typedef struct {
    int line;              /* 已渲染完的行数（下一行的 0-based 索引） */
    int cursor_line;       /* 命中时写入；未命中保持 -1 */
} render_ctx_t;

static size_t render_node(const hw_node_t *node,
                          const hw_node_t *cursor,
                          int depth,
                          const int *conts,
                          int is_last_at_level,
                          char *buf, size_t cap, size_t pos,
                          int first_line,
                          render_ctx_t *ctx) {
    if (!first_line) pos = buf_append(buf, cap, pos, "\n");

    if (node == cursor) ctx->cursor_line = ctx->line;

    for (int d = 0; d < depth; d++) {
        pos = buf_append(buf, cap, pos, conts[d] ? T_CONT : T_NONE);
    }
    pos = buf_append(buf, cap, pos, is_last_at_level ? T_LAST : T_MID);
    pos = buf_append(buf, cap, pos, " ");

    if (node->is_folded) {
        char prev[64];
        folded_preview(node->label, prev, sizeof(prev));
        pos = buf_append(buf, cap, pos, prev);
        pos = buf_append(buf, cap, pos, " …");
    } else {
        pos = buf_append(buf, cap, pos, node->label ? node->label : "");
    }

    ctx->line++;   /* 本节点占 1 个逻辑行（label 禁止换行，见 build_page_widgets） */

    if (!node->is_folded) {
        int next_conts[32];
        int nd = depth < 31 ? depth : 31;
        for (int d = 0; d < nd; d++) next_conts[d] = conts[d];
        next_conts[nd] = is_last_at_level ? 0 : 1;

        for (int i = 0; i < node->child_count; i++) {
            int child_is_last = (i == node->child_count - 1);
            pos = render_node(node->children[i], cursor, depth + 1,
                              next_conts, child_is_last,
                              buf, cap, pos, /*first_line*/0, ctx);
        }
    }
    return pos;
}

size_t hw_tree_render(const hw_node_t *root, const hw_node_t *cursor,
                      char *buf, size_t cap, int *out_cursor_line) {
    if (out_cursor_line) *out_cursor_line = -1;
    if (!buf || cap == 0) return 0;
    buf[0] = '\0';
    if (!root) return 0;
    int conts[32] = {0};
    size_t pos = 0;
    render_ctx_t ctx = { .line = 0, .cursor_line = -1 };
    for (int i = 0; i < root->child_count; i++) {
        int is_last = (i == root->child_count - 1);
        pos = render_node(root->children[i], cursor, 0, conts, is_last,
                          buf, cap, pos, /*first_line*/(i == 0), &ctx);
    }
    if (pos < cap) buf[pos] = '\0';
    if (out_cursor_line) *out_cursor_line = ctx.cursor_line;
    return pos;
}

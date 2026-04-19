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
    /* 以换行为行边界 */
    size_t i = 0;
    for (; label[i] && label[i] != '\n' && i < 45; i++) { }
    /* UTF-8 近似：最多 45 字节 ≈ 15 汉字 */
    if (i >= outcap) i = outcap - 1;
    memcpy(out, label, i);
    out[i] = '\0';
}

static size_t render_node(const hw_node_t *node,
                          const hw_node_t *cursor,
                          int depth,
                          char *buf, size_t cap, size_t pos) {
    /* 缩进 */
    for (int d = 0; d < depth; d++) {
        pos = buf_append(buf, cap, pos, "  ");
    }
    pos = buf_append(buf, cap, pos, (node == cursor) ? "▶ " : "• ");

    if (node->is_folded) {
        char prev[64];
        folded_preview(node->label, prev, sizeof(prev));
        pos = buf_append(buf, cap, pos, prev);
        pos = buf_append(buf, cap, pos, " …");
    } else {
        pos = buf_append(buf, cap, pos, node->label ? node->label : "");
    }
    pos = buf_append(buf, cap, pos, "\n");

    if (!node->is_folded) {
        for (int i = 0; i < node->child_count; i++) {
            pos = render_node(node->children[i], cursor, depth + 1,
                              buf, cap, pos);
        }
    }
    return pos;
}

size_t hw_tree_render(const hw_node_t *root, const hw_node_t *cursor,
                      char *buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    buf[0] = '\0';
    if (!root) return 0;
    size_t pos = 0;
    for (int i = 0; i < root->child_count; i++) {
        pos = render_node(root->children[i], cursor, 0, buf, cap, pos);
    }
    /* 去掉末尾换行（美观） */
    if (pos > 0 && buf[pos - 1] == '\n') { buf[pos - 1] = '\0'; pos--; }
    else if (pos < cap) buf[pos] = '\0';
    return pos;
}

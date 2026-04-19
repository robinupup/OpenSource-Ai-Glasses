/**
 * hw_tree —— 拍照搜题用的知识树数据结构
 *
 * 复刻自 lumina_edge/app/src/main/java/com/lumina_edge/ContentNode.kt +
 * GlassesUIManager.kt 中的子集：节点树 + 折叠 + 光标 + 多行文本渲染。
 *
 * 仅拍照搜题功能使用。与 lumina 端协议对齐：
 *   - 首轮 content 返回 {"nodes":[...]}，作为新顶层节点插入 root 下
 *   - expand 后 content 返回 {"children":[...]} 或 {"nodes":[...]}，挂到 cursor
 *
 * 线程模型：本模块不加锁；所有访问由 app_homework.c 内部 g_lock 串行化，
 * 渲染字符串拷贝到调用方 buffer 后再写入 LVGL label（走 app_ui_lock）。
 */
#ifndef MYAPP_HW_TREE_H
#define MYAPP_HW_TREE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hw_node {
    int                 id;
    char               *label;        /* strdup'd；root 的 label = NULL */
    int                 is_folded;    /* 1=折叠（子节点不渲染，仅显示自身首 15 字+…）*/
    struct hw_node     *parent;       /* root 的 parent = NULL */
    struct hw_node    **children;
    int                 child_count;
    int                 child_cap;
} hw_node_t;

/** 创建空虚拟根（id=0, label=NULL）。 */
hw_node_t *hw_tree_new_root(void);

/** 释放整棵树（递归 free label/children 数组/节点）。 */
void hw_tree_free(hw_node_t *root);

/** root->children 是否为空。 */
int hw_tree_is_empty(const hw_node_t *root);

/**
 * 把 JSON 字符串里的 {"nodes":[...]} 或 {"children":[...]} 数组中的所有
 * 节点作为 [parent] 的子节点追加进来（同名 label 不去重）。
 * 每个节点的 "children" 字段被递归展开。
 *
 * @return 成功追加的顶层节点数；负值表示 JSON 解析失败。
 */
int hw_tree_load_json_into(hw_node_t *parent, const char *json_str);

/**
 * 按深度优先顺序收集"可见"节点（祖先都未 folded）。
 * 不包含 root 自身。
 *
 * @param out  输出数组，容量 cap
 * @return 实际填入数量（可能被 cap 截断）
 */
int hw_tree_collect_visible(const hw_node_t *root, const hw_node_t **out, int cap);

/**
 * 深度优先，取 cursor 之后的下一个可见节点。若 cursor 为 NULL 或已是
 * 可见序列末尾，则返回首个可见节点（循环选择）。root 为空时返回 NULL。
 */
const hw_node_t *hw_tree_next_visible(const hw_node_t *root,
                                       const hw_node_t *cursor);

/**
 * 把整棵树渲染为多行字符串写入 [buf]（最多 cap-1 字节，保证 NUL 结尾）。
 * 规则：
 *   - 每层缩进 2 个空格
 *   - cursor 节点前缀 "▶ "；其它为 "• "
 *   - 折叠节点显示 "〈label〉 …"（不递归子节点）
 *   - 流式/加载中由上层自行追加 "…" 提示
 *
 * @return 实际写入字节数（不含结尾 NUL）
 */
size_t hw_tree_render(const hw_node_t *root, const hw_node_t *cursor,
                      char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* MYAPP_HW_TREE_H */

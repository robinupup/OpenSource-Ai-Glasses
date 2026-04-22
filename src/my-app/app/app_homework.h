/**
 * 拍照搜题（photo_search）控制器 —— ws://host:<photo_search_port>/photo_search
 *
 * 复刻自 lumina_edge 的知识树 + expand 交互：
 *   - 服务端首轮返回 {"nodes":[...]} → 渲染整棵树
 *   - 用户 expand 时，客户端发 {"type":"expand","data":"<selected>","uuid":"..."}
 *     服务端追加子节点 {"children":[...]} 挂到 cursor 下
 *
 * 按键映射（在本功能内部）：
 *   - 物理电源键       : on_confirm  = 收音开/关 toggle（首次开启时先拍一张照）
 *   - 触摸板 1 (NAV)   : on_nav_down = 光标向下（DFS 可见序列）
 *   - 触摸板 2 (ACT)   : on_act_expand = 对当前光标节点发起 expand
 *   - 触摸板 2 双击    : 退出功能（由 main.c 的双击检测处理）
 */
#ifndef MYAPP_APP_HOMEWORK_H
#define MYAPP_APP_HOMEWORK_H

#ifdef __cplusplus
extern "C" {
#endif

void app_homework_configure(const char *host, int port);
void app_homework_enter(void);
void app_homework_exit(void);

/** 电源键：收音 toggle。首次按下会先拍照、建立 uuid、送 image、开启 mic。 */
void app_homework_on_confirm(void);

/** 触摸板 1：光标向下（DFS 顺序，末尾回到首个）。 */
void app_homework_on_nav_down(void);

/** 触摸板 2：向服务端发 expand，请求为当前 cursor 挂子节点。 */
void app_homework_on_act_expand(void);

#ifdef __cplusplus
}
#endif

#endif

/**
 * 拍照搜题（photo_search）控制器 —— ws://host:<photo_search_port>/photo_search
 *
 * 复刻自 lumina_edge 的知识树 + expand 交互：
 *   - 服务端首轮返回 {"nodes":[...]} → 渲染整棵树
 *   - 用户 expand 时，客户端发 {"type":"expand","value":"<label>","data":"<uuid>"}
 *     服务端追加子节点 {"children":[...]} 挂到 cursor 下
 *
 * 触发方式：VAD 自动（复用 /oem/etc/silero_vad.ort，与英语对练同模型）
 *   - 进入页面后 mic 一直开启；Silero VAD 检测用户开口
 *   - SPEECH_START: 生成新 uuid + 异步拍一帧 JPEG + 发 image + 开始上行 PCM
 *   - SPEECH_CHUNK: 把 VAD 切好的 PCM 作为二进制帧直发（裸 PCM，无前缀）
 *   - SPEECH_END:   发 {"type":"mic","value":"off"} 触发服务端解题
 *
 * 按键映射（在本功能内部）：
 *   - 物理电源键       : on_confirm    = 暂停 / 恢复 VAD 监听（mute toggle）
 *   - 触摸板 1 (NAV)   : on_nav_down   = 光标向下（DFS 可见序列）
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

/** 电源键：暂停 / 恢复 VAD 监听（mute toggle）。暂停态下仍可 T1/长按浏览展开。 */
void app_homework_on_confirm(void);

/** 触摸板 1：光标向下（DFS 顺序，末尾回到首个）。 */
void app_homework_on_nav_down(void);

/** 触摸板 2：向服务端发 expand，请求为当前 cursor 挂子节点。 */
void app_homework_on_act_expand(void);

#ifdef __cplusplus
}
#endif

#endif

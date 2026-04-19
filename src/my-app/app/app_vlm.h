/**
 * 多模态百科（场景单词）控制器 —— ws://host:8002/vlm_talking
 *
 * 流程：
 *   IDLE    → CONFIRM: 连 WS + 拍照 + 发 {type:uuid}/{type:image} + 开麦 → REC
 *   REC     → CONFIRM: 关麦 + 发 {type:mic,value:off} → WAIT
 *   WAIT    → 收到 image_crop/content/done 后 → RESULT
 *   RESULT  → CONFIRM: 下一轮（自动回 IDLE 并立即开始）
 *   PAGE    → 退出，关 WS
 */
#ifndef MYAPP_APP_VLM_H
#define MYAPP_APP_VLM_H

#ifdef __cplusplus
extern "C" {
#endif

void app_vlm_configure(const char *host, int port);
void app_vlm_enter(void);
void app_vlm_exit(void);
void app_vlm_on_confirm(void);

#ifdef __cplusplus
}
#endif

#endif

/**
 * 实时翻译（英语对练）控制器 —— ws://host:8004/realtime_translate
 *
 * 按键语义：
 *   IDLE    → CONFIRM: 连 WS + 开麦，进入 REC
 *   REC     → CONFIRM: 关麦（不发控制帧），等待服务端 VAD 完成翻译
 *   RESULT  → CONFIRM: 再来一轮（回到 IDLE 立刻开麦）
 *   任意    → PAGE:    关 WS，退出回首页
 *
 * 服务端消息处理：
 *   {"type":"asr","text":...}       → 上屏 ASR
 *   {"type":"content","data":...}   → 追加到 content 区
 *   {"type":"done"}                 → 状态置 RESULT
 *   {"type":"interrupted"}          → 状态置 [被打断]
 */
#ifndef MYAPP_APP_TRANSLATE_H
#define MYAPP_APP_TRANSLATE_H

#ifdef __cplusplus
extern "C" {
#endif

/** 设定服务器 host/port；可在 main 启动时调用一次。 */
void app_translate_configure(const char *host, int port);

/** 进入功能；建立 WS 连接、重置 UI。 */
void app_translate_enter(void);

/** 退出功能；关闭 ws 和 mic。 */
void app_translate_exit(void);

/** GPIO CONFIRM 短按事件。 */
void app_translate_on_confirm(void);

#ifdef __cplusplus
}
#endif

#endif

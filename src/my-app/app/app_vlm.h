/**
 * 场景单词（多模态百科）控制器 —— ws://host:8002/scene_words
 *
 * 对齐 x_engine/src/apps/lumina/scene_words/serve.py 协议：
 *   - 上行：二进制 PCM(16k/1ch/16bit) | {type:audio|image|text|uuid|end}
 *   - 下行：{type:asr|intent|content|image_crop|done|error}
 *   - 句尾检测：服务端 server_vad 自动检测，客户端只需停止发音频；
 *     协议中没有 mic 控制指令。
 *
 * 流程：
 *   IDLE    → CONFIRM: 连 WS + 拍照 + 发 {type:uuid}/{type:image} + 开麦 → REC
 *   REC     → CONFIRM: 停麦 → WAIT（服务端 VAD 检测尾包后自动走业务）
 *   WAIT    → 收到 content/image_crop/done 后 → IDLE（可继续下一轮）
 *   PAGE    → 退出：发 {type:end} 并关 WS
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

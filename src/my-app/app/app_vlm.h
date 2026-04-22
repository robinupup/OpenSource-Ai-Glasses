/**
 * 场景单词（VAD 自动触发版）—— ws://host:8002/scene_words
 *
 * 对齐 x_engine/src/apps/lumina/scene_words/serve.py：
 *   - 上行：二进制 PCM(16k/1ch/16bit) | {type:image|uuid|text|end}
 *   - 下行：{type:asr|intent|content|image_crop|done|error}
 *   - 句尾检测：服务端 server_vad 自行收口；客户端 VAD 只作"一句话开始"触发。
 *
 * 交互：
 *   ENTER   → 连 WS + 起 Silero VAD + 开麦 → ARMED
 *   ARMED   → 监听中；VAD 判定 SPEECH_START
 *             → 后台拍一帧 JPEG，发 {type:uuid}/{type:image}
 *             → 同时把 PCM(二进制) 推给服务端 → STREAMING
 *   STREAM  → 持续推 PCM；收到 done → vad_reset → 回 ARMED
 *   CONFIRM → ARMED↔PAUSED 切换（临时关麦）
 *   EXIT    → 发 {type:end}，关 WS / VAD / MIC
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

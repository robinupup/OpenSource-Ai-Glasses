/**
 * 英语对练（english_practice，英语口语 AI Agent） —— ws://host:english_practice_port/
 *
 * 对端服务：examples/english_practice （device_main.py 的 C 版本）。
 * 协议（net/protocol.py）：
 *   Binary
 *     0x01  cloud → device  TTS PCM 块（24 kHz / 16-bit / mono）
 *     0x03  device → cloud  camera JPEG 帧（一次对话最多一帧）
 *     0x04  device → cloud  麦克风 PCM 块（16 kHz / 16-bit / mono，VAD 期间）
 *   JSON (文本帧)
 *     device → cloud：{"type":"audio_start","has_frame":bool}
 *                     {"type":"audio_end"}
 *                     {"type":"interrupt"}
 *     cloud → device：{"type":"agent_state","state":"listening"|"speaking"}
 *                     {"type":"interrupt_ack"}
 *                     {"type":"user_text","text":...}
 *                     {"type":"agent_text","text":...}
 *                     {"type":"partial_text","text":...}
 *                     {"type":"metrics",...}
 *
 * 本模块职责：
 *   1) 打开 WS，按 translate 的同款模式异步重连；
 *   2) 一旦 WS 就绪 + 用户 CONFIRM 启用，就开麦 + 跑 Silero VAD；
 *   3) VAD 判定 SPEECH_START → 先异步拍一帧 JPEG 推 0x03，再发 audio_start
 *      （若拍照仍未完成就保守地 has_frame=false，让云端走"纯语音"路径）；
 *      并在整个说话期间以 0x04 推 PCM；VAD 判定 SPEECH_END → 发 audio_end；
 *   4) 若用户在 agent speaking 期间插话（VAD 触发），发 interrupt + tts_stop
 *      抢断当前一段；
 *   5) user_text / agent_text / partial_text 直接上屏。
 *
 * 按键语义：
 *   IDLE        → CONFIRM: 连 WS + 开麦 + 启用 VAD，进入 ACTIVE
 *   ACTIVE      → CONFIRM: 关麦（不再收用户语音，不会插话），但 WS 保持；
 *                          再按一次回 ACTIVE
 *   任意        → PAGE:    关 mic / TTS / WS，退出回首页
 */
#ifndef MYAPP_APP_ENGLISH_H
#define MYAPP_APP_ENGLISH_H

#ifdef __cplusplus
extern "C" {
#endif

/** 设定服务器 host/port。main 启动时调一次即可。 */
void app_english_configure(const char *host, int port);

/** 进入功能：连 WS，UI 重置，不自动开麦。 */
void app_english_enter(void);

/** 退出功能：关 mic / TTS / WS。 */
void app_english_exit(void);

/** GPIO CONFIRM 短按：在 ACTIVE / 暂停 之间切换。 */
void app_english_on_confirm(void);

#ifdef __cplusplus
}
#endif

#endif

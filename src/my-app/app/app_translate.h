/**
 * 拟境英语（immersive_english）控制器 —— ws://host:<immersive_english_port>/immersive_english
 *
 * 对齐 x_engine/src/apps/lumina/immersive_english/serve.py 的完整协议：
 *
 *   上行（device → cloud）：
 *     Binary frame                       → PCM 音频（16kHz / mono / 16-bit）
 *     {"type":"audio","data":"<b64>"}    → 同上，b64 形式（二选一；本端用 binary）
 *     {"type":"config","tts":bool}       → 运行时开/关 TTS（可选，当前未使用）
 *     {"type":"end"}                     → 优雅结束会话
 *
 *   下行（cloud → device）：
 *     {"type":"asr","text":...}          → ASR 识别结果（原文）
 *     {"type":"content","data":...}      → 翻译流式输出
 *     {"type":"tts_start", sample_rate, format, channels, bits, seq, task_id}
 *                                        → 本句 TTS 开始，紧跟的 binary 属于此 seq
 *     Binary frame                       → TTS PCM 数据
 *     {"type":"tts_end","seq":N}         → 本句 TTS 自然结束
 *     {"type":"tts_stop","seq":N}        → 本句 TTS 被打断，需立即清空播放缓冲
 *     {"type":"tts_subtitle","seq":N,"subtitles":[...]}
 *                                        → 字级时间戳（仅 enable_subtitle=true 时）
 *     {"type":"done"}                    → 本句翻译完成（连续翻译：服务端不会主动断开）
 *     {"type":"interrupted"}             → 被新语音打断（随后会收到 tts_stop）
 *     {"type":"error","data":...}        → 错误
 *
 * 按键语义：
 *   IDLE    → CONFIRM: 连 WS + 开麦，进入 REC
 *   REC     → CONFIRM: 关麦 + 掐 TTS，不发控制帧（服务端自带 VAD 收尾），进入 WAIT
 *   WAIT    → 收到 done/interrupted 后 → IDLE（可继续下一轮）
 *   任意    → PAGE:    发 {"type":"end"} + 关 WS，退出回首页
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

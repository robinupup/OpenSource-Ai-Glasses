/**
 * my-app core - Silero VAD（设备端）
 *
 * 使用 onnxruntime C API 在 RV1106 的 Cortex-A7 上实时推理 Silero VAD v5：
 *   - 采样率：16 kHz
 *   - 每帧：512 samples = 32 ms（与 mic_pump 吐出的 1024 bytes/40ms 略有错位，
 *     vad 内部会按 512 samples 对齐，缓冲多余的 sample 到下一帧）
 *   - 单帧推理：~9.7 ms / 3.8 MB RSS（实测 RV1106）
 *
 * 用法：vad 以 "事件回调" 模式工作。调用方每次把最新采到的 PCM 片段（任意长度，
 * int16 LE mono 16k）丢给 `vad_feed`，vad 维护 SpeechState 状态机，把三类事件
 * 回调给上层：
 *
 *    VAD_EV_SPEECH_START   某帧 prob≥threshold 连续 N 次 → 判定开口（一次）
 *    VAD_EV_SPEECH_CHUNK   在开口状态下，把原始 PCM 回调出去（原样转发，
 *                         保证上层不必再缓存一份，可直接喂 WebSocket）
 *    VAD_EV_SPEECH_END     某帧 prob<threshold 持续 silence_ms → 判定闭口
 *
 * 为了和 device_main.py / english_practice 服务端的 VAD 行为对齐，默认：
 *    speech_threshold = 0.5
 *    min_speech_ms    = 250   (连续 8 帧 * 32ms 才判开口，防止瞬态噪声)
 *    min_silence_ms   = 600   (连续静音 600ms 才判闭口，允许句中换气)
 *
 * 线程模型：vad_feed 在哪条线程调用，就在哪条线程回调 on_event。调用方自行
 * 处理线程安全（on_event 里不要再阻塞回到 vad_feed）。
 */

#ifndef MYAPP_VAD_H
#define MYAPP_VAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VAD_EV_SPEECH_START = 1,
    VAD_EV_SPEECH_CHUNK = 2,
    VAD_EV_SPEECH_END   = 3,
} vad_event_t;

typedef void (*vad_event_cb_t)(vad_event_t ev,
                                const int16_t *pcm, size_t samples,
                                float prob,
                                void *user_data);

typedef struct {
    /** Silero .ort 模型绝对路径。必填。 */
    const char *model_path;
    /** VAD 帧触发阈值，默认 0.5（越高越难判为语音）。 */
    float speech_threshold;
    /** 连续语音 >= 该时长才发 SPEECH_START（ms），默认 250。 */
    int   min_speech_ms;
    /** 连续静音 >= 该时长才发 SPEECH_END（ms），默认 600。 */
    int   min_silence_ms;
    /** 事件回调。SPEECH_CHUNK 在开口状态把原始 PCM 转发给上层。 */
    vad_event_cb_t on_event;
    void *user_data;
} vad_config_t;

typedef struct vad vad_t;

/**
 * 初始化 vad，加载模型。模型不在时返回 NULL。调用一次即可，内部会起一条
 * 推理 session；之后 vad_feed 全部走同一条 session 复用内部 LSTM state。
 *
 * 线程安全：vad_create / vad_destroy 只在主线程调用；vad_feed / vad_reset
 * 必须由同一条线程调用（内部没有加锁）。
 */
vad_t *vad_create(const vad_config_t *cfg);

/** 重置内部 LSTM state + 状态机（新的对话回合开始前调一次）。 */
void   vad_reset(vad_t *v);

/**
 * 把新到的 PCM 送进来。samples 是 int16 数（不是字节数）。vad 会按 512 样本
 * 对齐切帧、逐帧跑 Silero、按策略发事件。samples 为 0 合法（no-op）。
 *
 * 注意：VAD_EV_SPEECH_CHUNK 转发的 PCM 以 *输入帧*（原始 samples）为单位，
 * 不会插值、不会丢弃。上层拿到就可以直接推 WS。
 */
void   vad_feed(vad_t *v, const int16_t *pcm, size_t samples);

/** 外部强制结束本轮说话（例如"用户关麦"按钮）：若当前在 SPEAKING，
 *  立即发一次 SPEECH_END（prob=0），然后回到 SILENCE 状态。 */
void   vad_force_end(vad_t *v);

/** 销毁 vad，释放 session。 */
void   vad_destroy(vad_t *v);

#ifdef __cplusplus
}
#endif

#endif

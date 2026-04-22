/**
 * my-app core - 流式 TTS PCM 播放器
 *
 * 为 immersive_english 等需要边收边放的 WebSocket 音频流设计：
 *   1. 收到 {"type":"tts_start",sample_rate,channels,bits,seq} → begin()
 *   2. 收到若干 binary frame（裸 PCM）          → feed(buf, n)
 *   3. 收到 {"type":"tts_end",seq}              → end()  （平滑播完残留）
 *   4. 收到 {"type":"tts_stop",seq} 或用户手动关麦 → stop() （立即丢弃）
 *
 * 实现要点：
 *   - 内部起一条 worker 线程，独占 /dev/snd/pcmC0D0p，从队列里取 chunk 写 ALSA。
 *   - feed() / stop() 线程安全，可在 WebSocket 回调线程里直接调用。
 *   - stop() 通过 SNDRV_PCM_IOCTL_DROP 立刻清空硬件缓冲，保证"被打断就闭嘴"。
 *   - 新的 begin() 会先抢占当前会话（相当于内建 stop()）。
 */

#ifndef MYAPP_TTS_PLAYER_H
#define MYAPP_TTS_PLAYER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int  tts_player_init(void);

/** 开启一段新的流式播放。返回 0 成功。若已有会话，会先被 stop 抢占。 */
int  tts_player_begin(int sample_rate, int channels, int bits);

/** 追加 PCM 数据（原始字节，无文件头）。线程安全。 */
int  tts_player_feed(const void *pcm, size_t len);

/** 服务端告知本段 TTS 已合成完毕：等队列播完后收尾。不阻塞。 */
int  tts_player_end(void);

/** 立即停止当前播放：丢弃队列 + SNDRV_PCM_IOCTL_DROP。不阻塞。 */
int  tts_player_stop(void);

/** 是否仍在活动（begin 之后，end/stop 完全收尾之前）。 */
int  tts_player_is_active(void);

void tts_player_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

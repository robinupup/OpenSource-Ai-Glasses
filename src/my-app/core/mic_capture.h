/**
 * my-app core - 麦克风音频流采集
 *
 * 通过 fork + exec `arecord`，从其 stdout 管道读取 PCM 数据。
 * 也可落盘为 WAV 文件。
 */

#ifndef MYAPP_MIC_CAPTURE_H
#define MYAPP_MIC_CAPTURE_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sample_rate; /* 默认 16000 */
    int channels;    /* 默认 1 */
    int bit_width;   /* 默认 16 */
} mic_params_t;

/** 句柄 */
typedef struct mic_stream mic_stream_t;

/** 启动采集流。成功返回句柄，失败返回 NULL。 */
mic_stream_t *mic_capture_start(const mic_params_t *params);

/**
 * 阻塞读取 PCM 数据（最多 size 字节）。
 * @return 实际读取字节数；0 表示 EOF；<0 表示错误。
 */
ssize_t mic_capture_read(mic_stream_t *stream, void *buf, size_t size);

/** 停止并释放流 */
void mic_capture_stop(mic_stream_t *stream);

/** 便捷接口：录制 N 秒到 WAV 文件（同步） */
int  mic_capture_to_wav(const char *output_path, int duration_sec,
                        const mic_params_t *params);

/* ==================== 回调式采集（适合送 WebSocket） ==================== */

/** PCM 数据回调。运行在采集后台线程中。 */
typedef void (*mic_pcm_cb_t)(const uint8_t *data, size_t len, void *user_data);

typedef struct mic_pump mic_pump_t;

/**
 * 启动一条后台采集 pump，每读到一段 PCM 就把原始数据回调给调用方。
 * chunk_bytes=0 表示按 ALSA 期间默认块返回；否则尽量按 chunk_bytes 对齐。
 * 16k/16bit/mono 下 40ms ≈ 1280 bytes。
 */
mic_pump_t *mic_pump_start(const mic_params_t *params, size_t chunk_bytes,
                            mic_pcm_cb_t cb, void *user_data);

/** 停止并释放 pump，阻塞至后台线程退出。 */
void mic_pump_stop(mic_pump_t *pump);

#ifdef __cplusplus
}
#endif

#endif

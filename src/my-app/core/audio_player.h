/**
 * my-app core - 喇叭/音频播放
 *
 * 直接调用 Rockchip 固件内置的 `aplay`/`amixer`，无需连接 ai-core。
 * 提供统一的播放接口；支持 wav 和 raw pcm。
 *
 * 事件：
 *   "audio/play_start"   payload: char file_path[]
 *   "audio/play_finish"  payload: char file_path[]
 */

#ifndef MYAPP_AUDIO_PLAYER_H
#define MYAPP_AUDIO_PLAYER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *file_path;
    int         volume;       /* 0-100, -1 表示不改变 */
    int         sample_rate;  /* 仅 raw 需要, wav 从头部解析 */
    int         channels;
    int         bit_width;    /* 8/16/24/32 */
    int         is_raw;       /* 1=raw pcm, 0=wav */
    int         async;        /* 1=异步(不等待结束), 0=同步 */
} audio_play_params_t;

/** 初始化（检查 aplay 是否可用） */
int  audio_player_init(void);

/** 按参数播放 */
int  audio_player_play(const audio_play_params_t *p);

/** 简化接口：同步播放一个 wav */
int  audio_player_play_wav(const char *file_path);

/** 异步播放 wav */
int  audio_player_play_wav_async(const char *file_path);

/** 停止当前所有播放 */
int  audio_player_stop(void);

/** 设置音量 0-100 */
int  audio_player_set_volume(int volume);

void audio_player_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

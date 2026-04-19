/**
 * my-app core - 相机模块（V4L2）
 *
 * 直接使用 Linux V4L2 接口从 /dev/videoX 获取一帧图像。
 * 默认格式优先 MJPG（便于保存/上传），可回退到 YUYV。
 */

#ifndef MYAPP_CAMERA_H
#define MYAPP_CAMERA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CAM_FMT_MJPG = 0,
    CAM_FMT_YUYV = 1,
    CAM_FMT_NV12 = 2,
} camera_fmt_t;

typedef struct {
    const char *device;   /* 默认 "/dev/video0" */
    int         width;    /* 默认 640 */
    int         height;   /* 默认 480 */
    int         format;   /* camera_fmt_t */
    int         buffers;  /* 默认 4 */
} camera_params_t;

typedef struct {
    uint8_t *data;       /* 已分配的缓冲（由调用方调用 camera_free_frame 释放） */
    size_t   size;
    int      width;
    int      height;
    int      format;
    uint64_t timestamp_us;
    uint32_t sequence;
} camera_frame_t;

typedef struct camera camera_t;

/** 打开相机并启动采集（后台队列循环） */
camera_t *camera_open(const camera_params_t *p);

/**
 * 获取下一帧。阻塞直到有帧或超时。
 * @param timeout_ms 超时毫秒，0 表示不阻塞（立刻返回），<0 表示无限等待
 * @return 0 成功（frame 已填充，调用方需 camera_free_frame 释放），<0 错误
 */
int camera_get_frame(camera_t *cam, camera_frame_t *frame, int timeout_ms);

/** 释放单帧缓冲 */
void camera_free_frame(camera_frame_t *frame);

/** 将帧保存到文件（MJPG 直接写入；其它格式写为 raw） */
int camera_save_frame(const camera_frame_t *frame, const char *path);

/** 关闭相机 */
void camera_close(camera_t *cam);

#ifdef __cplusplus
}
#endif

#endif

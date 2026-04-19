#include "camera.h"
#include "event_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>

/**
 * 本实现直接使用 V4L2。为兼容 Rockchip 平台：
 *   - 优先尝试 MPLANE (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
 *   - 回退 SPLANE (V4L2_BUF_TYPE_VIDEO_CAPTURE)
 *
 * 注：在 RV1103B 平台上，/dev/video0 是 rkcif（前端 RAW 输出），通常需要
 * rkaiq_3A_server / rkisp 配合。若只是想取 JPEG 快照，推荐走 rkipc 的
 * 快照接口；但此处仍保留标准 V4L2 路径以便兼容 UVC 摄像头或已配好管线
 * 的节点。
 */

#define CAM_MAX_BUFS   8
#define CAM_MAX_PLANES 4

typedef struct {
    void  *start[CAM_MAX_PLANES];
    size_t length[CAM_MAX_PLANES];
    int    n_planes;
} v4l2_buf_t;

struct camera {
    int              fd;
    int              width;
    int              height;
    int              format;
    int              n_bufs;
    int              mplane;       /* 1 = MPLANE, 0 = SPLANE */
    enum v4l2_buf_type buf_type;
    v4l2_buf_t       bufs[CAM_MAX_BUFS];
    uint32_t         sequence;
    int              streaming;
};

static uint32_t fourcc_of(int fmt) {
    switch (fmt) {
        case CAM_FMT_YUYV: return V4L2_PIX_FMT_YUYV;
        case CAM_FMT_NV12: return V4L2_PIX_FMT_NV12;
        case CAM_FMT_MJPG:
        default:           return V4L2_PIX_FMT_MJPEG;
    }
}

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

static int setup_splane(camera_t *cam, int w, int h, int fmt) {
    struct v4l2_format fmt_req;
    memset(&fmt_req, 0, sizeof(fmt_req));
    fmt_req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt_req.fmt.pix.width       = w;
    fmt_req.fmt.pix.height      = h;
    fmt_req.fmt.pix.pixelformat = fourcc_of(fmt);
    fmt_req.fmt.pix.field       = V4L2_FIELD_ANY;
    if (xioctl(cam->fd, VIDIOC_S_FMT, &fmt_req) < 0) return -1;
    cam->width  = fmt_req.fmt.pix.width;
    cam->height = fmt_req.fmt.pix.height;
    cam->mplane = 0;
    cam->buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    return 0;
}

static int setup_mplane(camera_t *cam, int w, int h, int fmt) {
    struct v4l2_format fmt_req;
    memset(&fmt_req, 0, sizeof(fmt_req));
    fmt_req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt_req.fmt.pix_mp.width       = w;
    fmt_req.fmt.pix_mp.height      = h;
    fmt_req.fmt.pix_mp.pixelformat = fourcc_of(fmt);
    fmt_req.fmt.pix_mp.field       = V4L2_FIELD_ANY;
    fmt_req.fmt.pix_mp.num_planes  = 1;
    if (xioctl(cam->fd, VIDIOC_S_FMT, &fmt_req) < 0) return -1;
    cam->width  = fmt_req.fmt.pix_mp.width;
    cam->height = fmt_req.fmt.pix_mp.height;
    cam->mplane = 1;
    cam->buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    return 0;
}

camera_t *camera_open(const camera_params_t *params) {
    camera_params_t p = {0};
    if (params) p = *params;
    const char *dev = p.device ? p.device : "/dev/video0";
    int w = p.width  > 0 ? p.width  : 640;
    int h = p.height > 0 ? p.height : 480;
    int n_bufs = p.buffers > 0 ? p.buffers : 4;
    if (n_bufs > CAM_MAX_BUFS) n_bufs = CAM_MAX_BUFS;

    int fd = open(dev, O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) {
        fprintf(stderr, "[camera] open %s: %s\n", dev, strerror(errno));
        return NULL;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "[camera] QUERYCAP failed\n"); close(fd); return NULL;
    }

    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                    ? cap.device_caps : cap.capabilities;
    int has_sp     = !!(caps & V4L2_CAP_VIDEO_CAPTURE);
    int has_mp     = !!(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE);
    int has_stream = !!(caps & V4L2_CAP_STREAMING);
    if (!has_stream || (!has_sp && !has_mp)) {
        fprintf(stderr, "[camera] %s: no capture/streaming capability (caps=0x%x)\n",
                dev, caps);
        close(fd); return NULL;
    }

    camera_t *cam = (camera_t *)calloc(1, sizeof(*cam));
    cam->fd = fd;
    cam->format = p.format;

    int set_ok = -1;
    if (has_mp) set_ok = setup_mplane(cam, w, h, p.format);
    if (set_ok != 0 && has_sp) set_ok = setup_splane(cam, w, h, p.format);
    if (set_ok != 0) {
        fprintf(stderr, "[camera] S_FMT failed on both planes\n");
        close(fd); free(cam); return NULL;
    }
    printf("[camera] %s: %dx%d %s (%s)\n", dev, cam->width, cam->height,
           cam->mplane ? "MPLANE" : "SPLANE",
           p.format == CAM_FMT_MJPG ? "MJPG" :
           p.format == CAM_FMT_NV12 ? "NV12" : "YUYV");

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = n_bufs;
    req.type   = cam->buf_type;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        fprintf(stderr, "[camera] REQBUFS failed: %s\n", strerror(errno));
        close(fd); free(cam); return NULL;
    }
    cam->n_bufs = req.count;

    for (int i = 0; i < cam->n_bufs; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[CAM_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type   = cam->buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (cam->mplane) {
            buf.m.planes = planes;
            buf.length   = 1;
        }
        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) goto fail;

        if (cam->mplane) {
            cam->bufs[i].n_planes = buf.length;
            for (unsigned int pi = 0; pi < buf.length && pi < CAM_MAX_PLANES; pi++) {
                cam->bufs[i].length[pi] = planes[pi].length;
                cam->bufs[i].start[pi]  = mmap(NULL, planes[pi].length,
                                               PROT_READ | PROT_WRITE, MAP_SHARED,
                                               fd, planes[pi].m.mem_offset);
                if (cam->bufs[i].start[pi] == MAP_FAILED) goto fail;
            }
        } else {
            cam->bufs[i].n_planes = 1;
            cam->bufs[i].length[0] = buf.length;
            cam->bufs[i].start[0]  = mmap(NULL, buf.length,
                                          PROT_READ | PROT_WRITE, MAP_SHARED,
                                          fd, buf.m.offset);
            if (cam->bufs[i].start[0] == MAP_FAILED) goto fail;
        }
        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) goto fail;
    }

    if (xioctl(fd, VIDIOC_STREAMON, &cam->buf_type) < 0) {
        fprintf(stderr, "[camera] STREAMON: %s\n", strerror(errno));
        goto fail;
    }
    cam->streaming = 1;
    return cam;

fail:
    for (int i = 0; i < cam->n_bufs; i++) {
        for (int pi = 0; pi < cam->bufs[i].n_planes; pi++) {
            if (cam->bufs[i].start[pi] && cam->bufs[i].start[pi] != MAP_FAILED)
                munmap(cam->bufs[i].start[pi], cam->bufs[i].length[pi]);
        }
    }
    close(fd); free(cam); return NULL;
}

int camera_get_frame(camera_t *cam, camera_frame_t *frame, int timeout_ms) {
    if (!cam || !frame) return -1;

    if (timeout_ms != 0) {
        fd_set fds;
        FD_ZERO(&fds); FD_SET(cam->fd, &fds);
        struct timeval tv = {0};
        struct timeval *ptv = NULL;
        if (timeout_ms > 0) {
            tv.tv_sec  = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            ptv = &tv;
        }
        int r = select(cam->fd + 1, &fds, NULL, NULL, ptv);
        if (r <= 0) return (r == 0) ? -ETIMEDOUT : -errno;
    }

    struct v4l2_buffer buf;
    struct v4l2_plane planes[CAM_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type   = cam->buf_type;
    buf.memory = V4L2_MEMORY_MMAP;
    if (cam->mplane) { buf.m.planes = planes; buf.length = CAM_MAX_PLANES; }
    if (xioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0) return -errno;

    size_t bytes = cam->mplane ? planes[0].bytesused : buf.bytesused;
    const void *src = cam->bufs[buf.index].start[0];
    uint8_t *copy = (uint8_t *)malloc(bytes);
    if (!copy) {
        xioctl(cam->fd, VIDIOC_QBUF, &buf);
        return -ENOMEM;
    }
    memcpy(copy, src, bytes);

    frame->data         = copy;
    frame->size         = bytes;
    frame->width        = cam->width;
    frame->height       = cam->height;
    frame->format       = cam->format;
    frame->timestamp_us = event_bus_now_us();
    frame->sequence     = ++cam->sequence;

    xioctl(cam->fd, VIDIOC_QBUF, &buf);

    struct { uint32_t seq; int w, h, fmt; size_t size; } meta = {
        frame->sequence, frame->width, frame->height, frame->format, frame->size
    };
    event_bus_publish("camera/frame", &meta, sizeof(meta));
    return 0;
}

void camera_free_frame(camera_frame_t *frame) {
    if (frame && frame->data) {
        free(frame->data);
        frame->data = NULL;
        frame->size = 0;
    }
}

int camera_save_frame(const camera_frame_t *frame, const char *path) {
    if (!frame || !frame->data || !path) return -1;
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t n = fwrite(frame->data, 1, frame->size, fp);
    fclose(fp);
    return (n == frame->size) ? 0 : -1;
}

void camera_close(camera_t *cam) {
    if (!cam) return;
    if (cam->streaming) {
        xioctl(cam->fd, VIDIOC_STREAMOFF, &cam->buf_type);
    }
    for (int i = 0; i < cam->n_bufs; i++) {
        for (int pi = 0; pi < cam->bufs[i].n_planes; pi++) {
            if (cam->bufs[i].start[pi] && cam->bufs[i].start[pi] != MAP_FAILED)
                munmap(cam->bufs[i].start[pi], cam->bufs[i].length[pi]);
        }
    }
    if (cam->fd >= 0) close(cam->fd);
    free(cam);
}

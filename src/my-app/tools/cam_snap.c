/*
 * cam_snap.c — 拍一张 1920x1080 NV12 落盘，并把 Y/U/V 均值 & 直方图统计打出来。
 * 用来诊断"拍出来偏绿"到底是 sensor 直出就这样、还是 nv12→JPEG 转换出的问题。
 *
 * 编译（用 my-app 同一套 toolchain）：
 *   $TC tools/cam_snap.c -O2 -o build/cam_snap
 *
 * 用法（adb shell）：
 *   /tmp/cam_snap /tmp/snap.nv12
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define CAM      "/dev/video7"
#define SUBDEV   "/dev/v4l-subdev2"
#define W        1920
#define H        1080

/* 与 my-app main.c 对齐：用 sensor subdev 设曝光/增益 */
static void set_ctrl(int fd, uint32_t id, int val, const char *name) {
    struct v4l2_control c = { .id = id, .value = val };
    if (ioctl(fd, VIDIOC_S_CTRL, &c) < 0) {
        fprintf(stderr, "[snap] set %s=%d failed: %s\n", name, val, strerror(errno));
    } else {
        printf("[snap] set %s=%d\n", name, val);
    }
}

int main(int argc, char **argv) {
    const char *out = argc >= 2 ? argv[1] : "/tmp/snap.nv12";

    int sfd = open(SUBDEV, O_RDWR);
    if (sfd >= 0) {
        int expo = 1700, gain = 600;
        const char *e = getenv("EXPO"); const char *g = getenv("GAIN");
        if (e) expo = atoi(e);
        if (g) gain = atoi(g);
        set_ctrl(sfd, V4L2_CID_EXPOSURE, expo, "exposure");
        set_ctrl(sfd, V4L2_CID_ANALOGUE_GAIN, gain, "analogue_gain");
        close(sfd);
    }

    int fd = open(CAM, O_RDWR);
    if (fd < 0) { perror(CAM); return 1; }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width       = W;
    fmt.fmt.pix_mp.height      = H;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return 1; }
    /* 回读实际生效的像素格式 */
    printf("[snap] actual fmt: 0x%08x (want NV12=0x%08x NV21=0x%08x)\n",
           fmt.fmt.pix_mp.pixelformat,
           V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV21);

    struct v4l2_requestbuffers req = {0};
    req.count  = 3;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return 1; }

    struct v4l2_buffer buf;
    struct v4l2_plane  planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory   = V4L2_MEMORY_MMAP;
    buf.index    = 0;
    buf.m.planes = planes;
    buf.length   = 1;
    if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) { perror("QUERYBUF"); return 1; }

    void *mem = mmap(NULL, buf.m.planes[0].length,
                     PROT_READ | PROT_WRITE, MAP_SHARED,
                     fd, buf.m.planes[0].m.mem_offset);
    if (mem == MAP_FAILED) { perror("mmap"); return 1; }

    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF"); return 1; }
    enum v4l2_buf_type btype = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd, VIDIOC_STREAMON, &btype) < 0) { perror("STREAMON"); return 1; }

    /* 丢弃前几帧，让 AE 收敛（虽然这里没 3A，也让 sensor 管线稳定） */
    int warmup = 3;
    const char *w_env = getenv("WARMUP");
    if (w_env) warmup = atoi(w_env);
    for (int i = 0; i < warmup; i++) {
        memset(&buf, 0, sizeof(buf)); memset(planes, 0, sizeof(planes));
        buf.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; buf.memory=V4L2_MEMORY_MMAP;
        buf.m.planes=planes; buf.length=1;
        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) { perror("DQBUF warmup"); return 1; }
        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    memset(&buf, 0, sizeof(buf)); memset(planes, 0, sizeof(planes));
    buf.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; buf.memory=V4L2_MEMORY_MMAP;
    buf.m.planes=planes; buf.length=1;
    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) { perror("DQBUF final"); return 1; }
    uint32_t bytes = buf.m.planes[0].bytesused;

    /* 落盘 */
    FILE *fp = fopen(out, "wb");
    if (!fp) { perror(out); return 1; }
    fwrite(mem, 1, bytes, fp);
    fclose(fp);

    /* 统计：Y 均值；UV 两种解读方式的均值 */
    const uint8_t *data = (const uint8_t *)mem;
    size_t ysize = (size_t)W * H;
    uint64_t sy = 0;
    for (size_t i = 0; i < ysize; i++) sy += data[i];
    double avg_y = (double)sy / ysize;

    const uint8_t *uv = data + ysize;
    size_t pairs = ysize / 4;  /* 420 下 UV 对数 */
    uint64_t sB = 0, sA = 0;
    for (size_t i = 0; i < pairs; i++) {
        sB += uv[2 * i + 0];
        sA += uv[2 * i + 1];
    }
    double avg_b = (double)sB / pairs;  /* byte0：若 NV12 是 Cb(U)，若 NV21 是 Cr(V) */
    double avg_a = (double)sA / pairs;  /* byte1：反之 */

    printf("[snap] wrote %s (%u B)\n", out, bytes);
    printf("[snap] avg Y = %.1f\n", avg_y);
    printf("[snap] avg byte0 = %.1f  (if NV12→Cb/U, if NV21→Cr/V)\n", avg_b);
    printf("[snap] avg byte1 = %.1f  (if NV12→Cr/V, if NV21→Cb/U)\n", avg_a);
    printf("[snap] 偏色判断：\n");
    printf("       - 若 byte0 代表 Cb(U)：Cb-128=%+.1f, Cr-128=%+.1f\n", avg_b-128, avg_a-128);
    printf("       - 若 byte0 代表 Cr(V)：Cr-128=%+.1f, Cb-128=%+.1f\n", avg_b-128, avg_a-128);
    printf("       (绿色偏：应看到 Cr<128 且 Cb<128 → Cr-128, Cb-128 都为负)\n");

    ioctl(fd, VIDIOC_STREAMOFF, &btype);
    munmap(mem, buf.m.planes[0].length);
    close(fd);
    return 0;
}

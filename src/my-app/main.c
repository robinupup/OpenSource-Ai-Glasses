/**
 * my-app - 全开源实现入口（含 LVGL UI + 拍照）
 *
 * 架构：
 *   core/event_bus + core/gpio_hub  -> 按键事件
 *   core/waveguide                  -> 4bpp 直驱光波导显示
 *   LVGL (third_party/lvgl)         -> UI 框架
 *   ui/ui_Screen1.c                 -> 四方框 (场景单词 / 拟境英语 / 拍照搜题 / 英语对练)
 *
 * 拍照实现：完全对照 src/ffm_launcher/launch.cpp：
 *   - /dev/v4l-subdev2 设置 曝光=1300, 模拟增益=200
 *   - /dev/video7      1920x1080 NV12 (V4L2 MPLANE) 单帧采集
 *   - NV12 原始数据  -> /tmp/myapp_frame_nv12.raw
 *   - libjpeg 软编码 -> /userdata/myapp/photos/photo_YYYYmmdd_HHMMSS.jpg
 *     （设备上没有 ffmpeg/cjpeg，只有 libjpeg.so.9，因此在进程内直接调用）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <stdint.h>
#include <linux/videodev2.h>

#include <jpeglib.h>

#include "core/myapp_core.h"
#include "core/config.h"
#include "core/net/ws_client.h"
#include "core/power_key.h"

#include "lvgl/lvgl.h"
#include "ui/ui.h"

#include "app/app_common.h"
#include "app/app_vlm.h"
#include "app/app_translate.h"
#include "app/app_homework.h"

/* ==================== 配置 ====================
 * WiFi SSID/密码 与 算法服务 IP/端口 等运行时配置，
 * 统一由 /oem/etc/myapp.conf 加载（见 core/config.h + scripts/myapp.conf.sample）。
 * 若该文件不存在，将使用 core/config.c 中的内置默认值。
 */

/* 触摸板映射（统一版，所有功能页共享）：
 *   触摸板 1 (GPIO  0, 右镜腿) = NAV
 *       首页：短按（<600ms 即抬起）→ 翻页（下一个高亮项）
 *             长按（≥600ms 持续）  → 进入当前高亮功能
 *       功能内：忽略
 *   触摸板 2 (GPIO 75, 左镜腿) = ACT
 *       首页：忽略
 *       功能内：单击（抬起即）→ 退出当前功能，返回首页
 *   电源键（物理按键，KEY_POWER via /dev/input）
 *       首页：忽略
 *       功能内：收音开关（toggle mic，由各 app 的 on_confirm 实现）
 *
 * 实测 (2026-04-17): my-app 用 core/gpio_hub.c (20ms sysfs 轮询) 直接
 *   读到 press/release 事件；release payload 自带 press_duration_ms，
 *   统一在 release 事件里按时长判定短按 / 长按。
 *
 * 为什么用 release 而不是 press：
 *   "按住"和"单击"在 press 瞬间无法区分；只有用户抬起手指时才知道按了多久。
 *   在 release 里用 press_duration_ms 阈值判断，用户抬手那一刻立即执行相应动作，
 *   UI 响应感跟单击一致，但多了"长按进入"这个维度。
 */
#define GPIO_NAV           0    /* 触摸板1 */
#define GPIO_ACT           75   /* 触摸板2 */
#define GPIO_POWER         POWER_KEY_GPIO  /* =1, 由 power_key 模块从 input 子系统转发 */

#define SCREEN_W           WAVEGUIDE_WIDTH
#define SCREEN_H           WAVEGUIDE_HEIGHT

/* 相机参数 (与 ffm_launcher/launch.cpp 保持一致) */
#define CAM_DEVICE         "/dev/video7"
#define CAM_SUBDEV         "/dev/v4l-subdev2"
#define CAM_WIDTH          1920
#define CAM_HEIGHT         1080
/* 曝光/增益默认值。可用环境变量 MYAPP_EXPOSURE / MYAPP_GAIN 现场覆盖调试：
 *   MYAPP_EXPOSURE=3000 MYAPP_GAIN=800 /oem/usr/bin/my-app
 * 1300/200 是 launch.cpp 的值，但室内光下明显偏暗 → 默认改到 3000/600。
 */
#define CAM_EXPOSURE_DEFAULT  3000
#define CAM_GAIN_DEFAULT      600

/* 拍照输出路径 */
#define PHOTO_DIR          "/userdata/myapp/photos"
#define PHOTO_NV12_TMP     "/tmp/myapp_frame_nv12.raw"

/* 首页四个选项 */
typedef enum {
    HOME_SCENE   = 0,   /* 场景单词 */
    HOME_TALK    = 1,   /* 拟境英语（原英语对练，后端仍为 realtime_translate）*/
    HOME_SEARCH  = 2,   /* 拍照搜题 */
    HOME_ENGLISH = 3,   /* 英语对练（新增，后端待接入）*/
    HOME_COUNT
} home_item_t;

/* 应用状态机：首页 / 进入功能中 */
typedef enum {
    APP_STATE_HOME = 0,       /* 首页：T1 短按=翻页，T1 长按=进入功能 */
    APP_STATE_IN_FUNCTION,    /* 功能内：T2 单击=退出，电源键=收音开关 */
} app_state_t;

/* 长按阈值（毫秒）。首页 T1 按住达到此值 **立即**判为长按（进入功能），
 * 不等用户抬手；抬手前到达阈值就直接进入，体验接近手机长按图标。
 * 未达到阈值就抬手 → 判短按（翻页）。 */
#define LONG_PRESS_MS 600

static volatile int g_running = 1;
static volatile int g_menu_index = HOME_SCENE;
static volatile app_state_t g_app_state = APP_STATE_HOME;
static volatile int g_active_function = -1;   /* 进入功能后记录的 home_item_t */
/* T1 按下瞬间的时间戳（首页 + 功能内共用）：
 *   = 0      未按 / 已按下但长按已触发（release 不再当短按处理）
 *   != 0     按下中，主循环到 LONG_PRESS_MS 后触发长按动作并清零。
 * 首页长按=进入功能；功能内长按=调 g_apps[idx].t1_long（可选）。 */
static volatile uint64_t g_t1_press_ms = 0;
static pthread_mutex_t g_ui_mutex = PTHREAD_MUTEX_INITIALIZER;
/* 避免用户连按导致并发 V4L2 打开 */
static pthread_mutex_t g_cam_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ==================== LVGL → waveguide flush ==================== */
static uint8_t g_fb[WAVEGUIDE_FRAME_SIZE];

static void lv_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;

    for (int32_t y = 0; y < h; y++) {
        int fb_y = area->y1 + y;
        const lv_color_t *src = color_p + y * w;
        uint8_t *dst_row = g_fb + fb_y * (SCREEN_W / 2);
        for (int32_t x = 0; x < w; x++) {
            int fb_x = area->x1 + x;
            uint8_t gray4 = (src[x].full & 0xF0) >> 4;
            int off = fb_x / 2;
            if ((fb_x & 1) == 0) {
                dst_row[off] = (uint8_t)((gray4 << 4) | (dst_row[off] & 0x0F));
            } else {
                dst_row[off] = (uint8_t)((dst_row[off] & 0xF0) | (gray4 & 0x0F));
            }
        }
    }

    if (lv_disp_flush_is_last(drv)) {
        waveguide_commit_framebuffer(g_fb);
    }
    lv_disp_flush_ready(drv);
}

static int lvgl_setup(void) {
    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf1[SCREEN_W * 20];
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, SCREEN_W * 20);

    static lv_disp_drv_t drv;
    lv_disp_drv_init(&drv);
    drv.hor_res  = SCREEN_W;
    drv.ver_res  = SCREEN_H;
    drv.flush_cb = lv_flush_cb;
    drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&drv);
    return 0;
}

/* ==================== UI 控制 ==================== */
static void update_home_highlight(int index) {
    if (!ui_SelectionRect) return;
    /* 与 ui_Screen1.c 中 BOX_X0=16、BOX_DX=156、BOX_Y=120 保持一致 */
    static const int kXs[HOME_COUNT] = {
        16  - 4,
        172 - 4,
        328 - 4,
        484 - 4,
    };
    if (index < 0 || index >= HOME_COUNT) return;
    lv_obj_set_pos(ui_SelectionRect, kXs[index], 120 - 4);
}

static void hide_all_function_pages(void) {
    if (ui_subMenu)                  lv_obj_add_flag(ui_subMenu,                  LV_OBJ_FLAG_HIDDEN);
    if (ui_SceneWordsContainer)      lv_obj_add_flag(ui_SceneWordsContainer,      LV_OBJ_FLAG_HIDDEN);
    if (ui_EnglishTalkContainer)     lv_obj_add_flag(ui_EnglishTalkContainer,     LV_OBJ_FLAG_HIDDEN);
    if (ui_PhotoSearchContainer)     lv_obj_add_flag(ui_PhotoSearchContainer,     LV_OBJ_FLAG_HIDDEN);
    if (ui_EnglishPracticeContainer) lv_obj_add_flag(ui_EnglishPracticeContainer, LV_OBJ_FLAG_HIDDEN);
}

static void show_home(void) {
    if (ui_WaitBtContainer)        lv_obj_add_flag(ui_WaitBtContainer,        LV_OBJ_FLAG_HIDDEN);
    hide_all_function_pages();
    if (ui_VideoContainer)         lv_obj_clear_flag(ui_VideoContainer,       LV_OBJ_FLAG_HIDDEN);
    update_home_highlight(g_menu_index);
}

/* 显示指定功能页面（先隐藏首页和其他功能页） */
static void show_function_page(lv_obj_t *page) {
    if (!page) return;
    if (ui_VideoContainer) lv_obj_add_flag(ui_VideoContainer, LV_OBJ_FLAG_HIDDEN);
    hide_all_function_pages();
    lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
}

/* ==================== 相机：V4L2 采集 + ffmpeg 转 JPEG ==================== */

/* mkdir -p 实现（不依赖 system） */
static int mkdir_p(const char *path) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* 设置 ISP 曝光/增益。可由环境变量 MYAPP_EXPOSURE / MYAPP_GAIN 覆盖。 */
static int set_camera_controls(void) {
    int fd = open(CAM_SUBDEV, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[cam] open %s failed: %s\n", CAM_SUBDEV, strerror(errno));
        return -1;
    }

    int exposure = CAM_EXPOSURE_DEFAULT;
    int gain     = CAM_GAIN_DEFAULT;
    const char *e_env = getenv("MYAPP_EXPOSURE");
    const char *g_env = getenv("MYAPP_GAIN");
    if (e_env && *e_env) exposure = atoi(e_env);
    if (g_env && *g_env) gain     = atoi(g_env);

    struct v4l2_control ctrl;
    int rc = 0;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id    = V4L2_CID_EXPOSURE;
    ctrl.value = exposure;
    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        fprintf(stderr, "[cam] set exposure=%d failed: %s\n", exposure, strerror(errno));
        rc = -1;
    }

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id    = V4L2_CID_ANALOGUE_GAIN;
    ctrl.value = gain;
    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        fprintf(stderr, "[cam] set gain=%d failed: %s\n", gain, strerror(errno));
        rc = -1;
    }

    printf("[cam] exposure=%d gain=%d\n", exposure, gain);
    close(fd);
    return rc;
}

/* 采集一帧 NV12 到文件，完全对照 launch.cpp:capture_nv12_frame() */
static int capture_nv12_to_file(const char *out_path) {
    int fd = open(CAM_DEVICE, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[cam] open %s failed: %s\n", CAM_DEVICE, strerror(errno));
        return -1;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width       = CAM_WIDTH;
    fmt.fmt.pix_mp.height      = CAM_HEIGHT;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "[cam] VIDIOC_S_FMT failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = 3;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "[cam] VIDIOC_REQBUFS failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    struct v4l2_buffer buf;
    struct v4l2_plane  planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory   = V4L2_MEMORY_MMAP;
    buf.index    = 0;
    buf.m.planes = planes;
    buf.length   = 1;

    if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
        fprintf(stderr, "[cam] VIDIOC_QUERYBUF failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    void *mem = mmap(NULL, buf.m.planes[0].length,
                     PROT_READ | PROT_WRITE, MAP_SHARED,
                     fd, buf.m.planes[0].m.mem_offset);
    if (mem == MAP_FAILED) {
        fprintf(stderr, "[cam] mmap failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "[cam] VIDIOC_QBUF failed: %s\n", strerror(errno));
        munmap(mem, buf.m.planes[0].length);
        close(fd);
        return -1;
    }

    enum v4l2_buf_type btype = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd, VIDIOC_STREAMON, &btype) < 0) {
        fprintf(stderr, "[cam] VIDIOC_STREAMON failed: %s\n", strerror(errno));
        munmap(mem, buf.m.planes[0].length);
        close(fd);
        return -1;
    }

    /* 取一帧 */
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory   = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length   = 1;

    int rc = 0;
    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        fprintf(stderr, "[cam] VIDIOC_DQBUF failed: %s\n", strerror(errno));
        rc = -1;
        goto out_streamoff;
    }

    {
        uint32_t bytes = buf.m.planes[0].bytesused;
        FILE *fp = fopen(out_path, "wb");
        if (!fp) {
            fprintf(stderr, "[cam] fopen %s failed: %s\n", out_path, strerror(errno));
            rc = -1;
            goto out_streamoff;
        }
        if (fwrite(mem, 1, bytes, fp) != bytes) {
            fprintf(stderr, "[cam] fwrite short: %s\n", strerror(errno));
            rc = -1;
        }
        fclose(fp);
        printf("[cam] NV12 frame saved: %s (%u bytes)\n", out_path, bytes);
    }

out_streamoff:
    ioctl(fd, VIDIOC_STREAMOFF, &btype);
    munmap(mem, buf.m.planes[0].length);
    close(fd);
    return rc;
}

/*
 * 用 libjpeg 把 NV12 raw 软编码为 JPEG。
 * NV12 布局：
 *   - Y 平面：w*h 字节
 *   - UV 交错平面：(w/2)*(h/2)*2 字节，每 2 字节 = 一组 Cb/Cr
 * 送入 libjpeg 时把 UV 做 2x2 最近邻上采样到与 Y 相同分辨率，
 * 然后以 JCS_YCbCr 3 通道交错送进去 (每行 3*w 字节)。
 * 设备无 ffmpeg，这是最小依赖方案（只需 libjpeg.so.9）。
 */
static int nv12_to_jpeg(const char *nv12_path, const char *jpeg_path,
                         int w, int h, int quality) {
    FILE *in = fopen(nv12_path, "rb");
    if (!in) { fprintf(stderr, "[cam] open %s: %s\n", nv12_path, strerror(errno)); return -1; }
    size_t ysize  = (size_t)w * h;
    size_t uvsize = ysize / 2;
    uint8_t *y  = (uint8_t *)malloc(ysize);
    uint8_t *uv = (uint8_t *)malloc(uvsize);
    uint8_t *row = (uint8_t *)malloc((size_t)w * 3);
    if (!y || !uv || !row) {
        fprintf(stderr, "[cam] malloc failed\n");
        free(y); free(uv); free(row); fclose(in); return -1;
    }
    if (fread(y, 1, ysize, in) != ysize || fread(uv, 1, uvsize, in) != uvsize) {
        fprintf(stderr, "[cam] NV12 file too small\n");
        free(y); free(uv); free(row); fclose(in); return -1;
    }
    fclose(in);

    FILE *out = fopen(jpeg_path, "wb");
    if (!out) {
        fprintf(stderr, "[cam] open %s: %s\n", jpeg_path, strerror(errno));
        free(y); free(uv); free(row); return -1;
    }

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, out);

    cinfo.image_width      = w;
    cinfo.image_height     = h;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_YCbCr;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < (JDIMENSION)h) {
        int line = cinfo.next_scanline;
        const uint8_t *yrow  = y  + line * w;
        const uint8_t *uvrow = uv + (line / 2) * w;  /* 交错 UV，每 2 字节一组 */
        uint8_t *dst = row;
        /*
         * 实测 RV1103B / rkcif 在此路径下实际输出 NV21（V 在前 U 在后），
         * 因此这里的字节顺序是 Cr 先、Cb 后。若以后换成真正的 NV12，把两行互换即可。
         */
        for (int x = 0; x < w; x++) {
            int uv_idx = (x / 2) * 2;
            *dst++ = yrow[x];
            *dst++ = uvrow[uv_idx + 1];  /* Cb (NV21: 奇数字节) */
            *dst++ = uvrow[uv_idx];      /* Cr (NV21: 偶数字节) */
        }
        JSAMPROW r = row;
        jpeg_write_scanlines(&cinfo, &r, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(out);

    free(y); free(uv); free(row);
    return 0;
}

/* 执行"拍照搜题"完整动作 —— 采集 + 存盘 */
static int capture_and_save_photo(char *out_jpeg, size_t out_len) {
    pthread_mutex_lock(&g_cam_mutex);

    if (mkdir_p(PHOTO_DIR) != 0) {
        fprintf(stderr, "[cam] mkdir_p %s failed: %s\n", PHOTO_DIR, strerror(errno));
        pthread_mutex_unlock(&g_cam_mutex);
        return -1;
    }

    (void)set_camera_controls();   /* 非致命 */

    if (capture_nv12_to_file(PHOTO_NV12_TMP) != 0) {
        pthread_mutex_unlock(&g_cam_mutex);
        return -1;
    }

    char jpeg_path[256];
    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_local);
    snprintf(jpeg_path, sizeof(jpeg_path),
             "%s/photo_%s.jpg", PHOTO_DIR, ts);

    if (nv12_to_jpeg(PHOTO_NV12_TMP, jpeg_path, CAM_WIDTH, CAM_HEIGHT, 95) != 0) {
        pthread_mutex_unlock(&g_cam_mutex);
        return -1;
    }
    unlink(PHOTO_NV12_TMP);

    if (out_jpeg && out_len > 0) {
        snprintf(out_jpeg, out_len, "%s", jpeg_path);
    }
    printf("[cam] photo saved: %s\n", jpeg_path);

    pthread_mutex_unlock(&g_cam_mutex);
    return 0;
}

/* 对外（app_*.c）：拍照落盘并返回 jpeg 路径。 */
int myapp_take_photo(char *out_path, size_t out_len) {
    return capture_and_save_photo(out_path, out_len);
}

/* ==================== 三个功能页 UI 扩展 ==================== */
/* 为每个功能容器延迟创建 asr/content/status/tree/crop 子控件，并组装 app_ui_t。*/
typedef struct {
    lv_obj_t *asr;
    lv_obj_t *content;
    lv_obj_t *status;
    lv_obj_t *tree_labels[3];
    lv_obj_t *crop_img;
    /* 拍照搜题专用：整棵树渲染到 content（label 宽度超大+禁止折行），
     * 外层 tree_wrap 负责裁剪和滚动；tree_cursor_rect 作为光标行高亮矩形。 */
    lv_obj_t *tree_wrap;
    lv_obj_t *tree_cursor_rect;
} page_widgets_t;

static page_widgets_t g_vlm_w, g_tr_w, g_hw_w, g_ep_w;

static lv_obj_t *mk_label(lv_obj_t *parent, int y, lv_color_t color,
                          const lv_font_t *font) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, 600);
    lv_obj_set_pos(lbl, 20, y);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(lbl, "");
    return lbl;
}

static void build_page_widgets(lv_obj_t *container, page_widgets_t *w,
                                int with_crop, int with_tree) {
    /* 顶部 ASR（标题下方）；中间 content；底部 status（hint 上方）
     * 约定：功能名 (48) > 所有内容文字 (30 统一) = 退出提示 (30) */
    w->asr     = mk_label(container, 80,  lv_color_make(200, 200, 200),
                           &ui_font_alibaba_30);
    w->content = mk_label(container, 130, lv_color_white(),
                           &ui_font_alibaba_30);
    w->status  = mk_label(container, 390, lv_color_make(150, 150, 150),
                           &ui_font_alibaba_30);

    if (with_tree) {
        /* 拍照搜题整体结构：
         *   tree_wrap (600x300, 可纵向滚动)
         *     ├─ tree_cursor_rect  灰色矩形，悬浮在光标行上 → "整行高亮"
         *     └─ content label     整棵树的多行文本（禁止折行，宽度 1200
         *                          避免 LVGL 自动 wrap；超宽部分被 wrap 裁剪）
         * 不再使用 ▶ 箭头、recolor；光标高亮纯靠矩形定位。字数过多时
         * app_ui_set_tree_cursor 调 lv_obj_scroll_to_view 让 wrap 跟随光标翻页。 */
        lv_obj_del(w->content);   /* 换掉默认的 content label */

        lv_obj_t *wrap = lv_obj_create(container);
        lv_obj_set_pos(wrap, 20, 90);
        lv_obj_set_size(wrap, 600, 300);
        lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wrap, 0, 0);
        lv_obj_set_style_pad_all(wrap, 0, 0);
        lv_obj_set_style_radius(wrap, 0, 0);
        /* 关键：LVGL 的 lv_obj_create 默认走 theme 里的 lv_font_default
         * （通常是 montserrat_14，不含中文），子节点会通过 text_font 样式继承
         * 拿到这个默认字体。把 wrap 的 text_font 显式设成 alibaba_30，
         * 否则子 label 在还没生效 local style 之前会按默认字体找不到汉字 → 方框。*/
        lv_obj_set_style_text_font(wrap, &ui_font_alibaba_30, 0);
        lv_obj_set_style_text_color(wrap, lv_color_white(), 0);
        lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLL_ELASTIC);
        lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLL_MOMENTUM);
        lv_obj_add_flag  (wrap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(wrap, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(wrap, LV_SCROLLBAR_MODE_OFF);

        /* 光标高亮矩形（先隐藏，收到数据后由 app_ui_set_tree_cursor 定位） */
        lv_obj_t *rect = lv_obj_create(wrap);
        lv_obj_set_size(rect, 600, 35);
        lv_obj_set_pos(rect, 0, 0);
        lv_obj_set_style_bg_color(rect, lv_color_make(90, 90, 90), 0);
        lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(rect, 0, 0);
        lv_obj_set_style_radius(rect, 4, 0);
        lv_obj_set_style_pad_all(rect, 0, 0);
        lv_obj_clear_flag(rect, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag  (rect, LV_OBJ_FLAG_HIDDEN);

        /* 树文本 label：宽度给到 1200 让 LVGL 完全不 wrap，单行 = 单节点 */
        lv_obj_t *tl = lv_label_create(wrap);
        lv_label_set_long_mode(tl, LV_LABEL_LONG_CLIP);
        lv_obj_set_pos(tl, 0, 0);
        lv_obj_set_width(tl, 1200);
        lv_obj_set_style_text_color(tl, lv_color_white(), 0);
        lv_obj_set_style_text_font(tl, &ui_font_alibaba_30, 0);
        lv_obj_set_style_text_align(tl, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(tl, "");

        /* rect 放在 label 后方，让文字覆盖在高亮框之上（但灰色背景仍可见） */
        lv_obj_move_background(rect);

        w->content           = tl;
        w->tree_wrap         = wrap;
        w->tree_cursor_rect  = rect;
        for (int i = 0; i < 3; i++) w->tree_labels[i] = NULL;
    }
    if (with_crop) {
        w->crop_img = lv_img_create(container);
        lv_obj_set_pos(w->crop_img, 460, 120);
        lv_obj_set_size(w->crop_img, 160, 160);
    }
}

static void install_app_ui(const page_widgets_t *w) {
    app_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui.asr_label     = w->asr;
    ui.content_label = w->content;
    ui.status_label  = w->status;
    for (int i = 0; i < 3; i++) ui.tree_labels[i] = w->tree_labels[i];
    ui.crop_img          = w->crop_img;
    ui.tree_wrap         = w->tree_wrap;
    ui.tree_cursor_rect  = w->tree_cursor_rect;
    app_common_set_ui(&ui);
}

/* 给 app_common 用的 LVGL 互斥回调包装 */
static void ui_lock_wrap(void)   { pthread_mutex_lock(&g_ui_mutex); }
static void ui_unlock_wrap(void) { pthread_mutex_unlock(&g_ui_mutex); }

/* ==================== 功能进入/退出 ==================== */
static const char *function_name(int idx) {
    switch (idx) {
        case HOME_SCENE:   return "场景单词";
        case HOME_TALK:    return "拟境英语";
        case HOME_SEARCH:  return "拍照搜题";
        case HOME_ENGLISH: return "英语对练";
        default:           return "?";
    }
}

/*
 * 菜单与 lumina 子应用映射（按用户需求）：
 *   HOME_SCENE   (场景单词)  → 多模态百科 (vlm_talking    :8002)
 *   HOME_TALK    (拟境英语)  → 实时翻译   (realtime_translate :8004)
 *   HOME_SEARCH  (拍照搜题)  → 拍照搜题   (homework_finding   :8003)
 *   HOME_ENGLISH (英语对练)  → 后端待接入，目前仅显示页面标题与退出提示
 */

typedef struct {
    void (*enter)(void);
    void (*exit)(void);
    void (*confirm)(void);   /* 功能内电源键 → toggle 收音 */
    /* 功能内触摸板 1：可选的短按 / 长按回调。为 NULL 表示该 app 不响应。
     * 当前只有拍照搜题注册：短按=光标下移，长按=展开当前节点。 */
    void (*t1_short)(void);
    void (*t1_long)(void);
    const page_widgets_t *widgets;
    lv_obj_t *container;
    const char *title;
} app_entry_t;

static app_entry_t g_apps[HOME_COUNT];

static void enter_function(int idx) {
    if (idx < 0 || idx >= HOME_COUNT) return;
    g_app_state       = APP_STATE_IN_FUNCTION;
    g_active_function = idx;

    printf("[app] ====> 进入功能：%s\n", function_name(idx));

    pthread_mutex_lock(&g_ui_mutex);
    show_function_page(g_apps[idx].container);
    install_app_ui(g_apps[idx].widgets);
    pthread_mutex_unlock(&g_ui_mutex);

    if (g_apps[idx].enter) g_apps[idx].enter();
}

static void exit_function(void) {
    int idx = g_active_function;
    printf("[app] <==== 退出功能：%s\n", function_name(idx));
    if (idx >= 0 && idx < HOME_COUNT && g_apps[idx].exit) g_apps[idx].exit();

    g_active_function = -1;
    g_app_state       = APP_STATE_HOME;

    pthread_mutex_lock(&g_ui_mutex);
    show_home();
    pthread_mutex_unlock(&g_ui_mutex);
}

/* ==================== GPIO 事件 ==================== */
/*
 * 全局按键语义（所有功能页一致）：
 *   HOME:         T1 press  → 记录按下时刻，主循环到 LONG_PRESS_MS 立即进入功能
 *                 T1 release→ 若长按尚未触发：按 duration<阈值 视为短按=翻页
 *                                            duration>=阈值 视为长按=进入（兜底）
 *                 T2 release→ 忽略
 *                 POWER     → 忽略
 *   IN_FUNCTION:  T1        → 忽略（预留）
 *                 T2 release→ 退出回首页
 *                 POWER press→ 透传 app.confirm（= 收音 toggle）
 *
 * 长按立即触发靠主循环里的 poll_long_press_t1() 每 5ms 检查一次；
 * 正常情况下长按触发后 g_t1_press_ms 清零，随后的 release 被丢弃。
 */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/* 独立线程轮询 T1 长按。
 * 为什么不放在 LVGL 主循环里？
 *   enter_function() 会同步调用 app 的 enter()，而 app_translate_enter /
 *   app_vlm_enter 里 ensure_ws_open() 最多会阻塞 2s 等 WebSocket 握手。
 *   如果卡在主线程上，lv_timer_handler 就停了，屏幕在这 2s 内完全不刷新，
 *   用户看到的就是"长按之后界面没切"。放到独立线程等价于老版本在
 *   event_bus 派发线程里调 enter_function 的行为，UI 能照常渲染。 */
static pthread_t    g_lp_thread;
static volatile int g_lp_running = 0;

static void *longpress_thread_main(void *arg) {
    (void)arg;
    while (g_lp_running) {
        uint64_t t = g_t1_press_ms;
        if (t != 0 && now_ms() - t >= LONG_PRESS_MS) {
            g_t1_press_ms = 0;   /* release 看到 0 就不再当短按处理 */
            if (g_app_state == APP_STATE_HOME) {
                pthread_mutex_lock(&g_ui_mutex);
                int idx = g_menu_index;
                pthread_mutex_unlock(&g_ui_mutex);
                enter_function(idx);
            } else {
                /* 功能内长按：派发给 app，例如拍照搜题 = expand */
                int idx = g_active_function;
                if (idx >= 0 && idx < HOME_COUNT && g_apps[idx].t1_long) {
                    g_apps[idx].t1_long();
                }
            }
        }
        usleep(10 * 1000);
    }
    return NULL;
}

/* press 事件：首页 T1 开始计时；电源键功能内切麦。*/
static void on_gpio_press(const event_t *evt, void *user_data) {
    (void)user_data;
    const gpio_event_payload_t *p = (const gpio_event_payload_t *)evt->payload;
    printf("[app] GPIO%d PRESS (state=%d)\n", p->gpio, (int)g_app_state);

    if (p->gpio == GPIO_NAV) {            /* 触摸板 1 按下：开始长按计时
                                           * 首页 & 功能内都需要计时——长按线程
                                           * 会按当前 app_state 派发到不同动作。 */
        uint64_t n = now_ms();
        g_t1_press_ms = (n == 0 ? 1 : n);
        return;
    }

    if (p->gpio == GPIO_POWER) {          /* 物理电源键：功能内=收音 toggle */
        if (g_app_state == APP_STATE_IN_FUNCTION) {
            int idx = g_active_function;
            if (idx >= 0 && idx < HOME_COUNT && g_apps[idx].confirm) {
                g_apps[idx].confirm();
            }
        }
    }
}

/* release 事件：T1 若长按未触发就按 duration 判定；T2 抬起在功能内=退出。*/
static void on_gpio_release(const event_t *evt, void *user_data) {
    (void)user_data;
    const gpio_event_payload_t *p = (const gpio_event_payload_t *)evt->payload;
    int dur = p->press_duration_ms;
    printf("[app] GPIO%d RELEASE dur=%dms (state=%d)\n",
           p->gpio, dur, (int)g_app_state);

    if (p->gpio == GPIO_NAV) {            /* 触摸板 1 抬起 */
        uint64_t t = g_t1_press_ms;
        g_t1_press_ms = 0;
        if (t == 0) return;                /* 长按已在 poll 里触发，这里忽略 */

        int is_long = (dur >= LONG_PRESS_MS);
        if (g_app_state == APP_STATE_HOME) {
            if (is_long) {
                pthread_mutex_lock(&g_ui_mutex);
                int idx = g_menu_index;
                pthread_mutex_unlock(&g_ui_mutex);
                enter_function(idx);
            } else {
                pthread_mutex_lock(&g_ui_mutex);
                g_menu_index = (g_menu_index + 1) % HOME_COUNT;
                update_home_highlight(g_menu_index);
                pthread_mutex_unlock(&g_ui_mutex);
            }
        } else {
            /* 功能内：短按→t1_short（拍照搜题=下移一行）；长按兜底→t1_long */
            int idx = g_active_function;
            if (idx >= 0 && idx < HOME_COUNT) {
                void (*cb)(void) = is_long ? g_apps[idx].t1_long
                                           : g_apps[idx].t1_short;
                if (cb) cb();
            }
        }
        return;
    }

    if (p->gpio == GPIO_ACT) {            /* 触摸板 2 抬起 */
        if (g_app_state == APP_STATE_IN_FUNCTION) exit_function();
        return;
    }
    /* GPIO_POWER 在 press 里已处理。*/
}

/* ==================== 信号 ==================== */
static void on_signal(int sig) { (void)sig; g_running = 0; }

/* ==================== 主逻辑 ==================== */
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    setbuf(stdout, NULL);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* SIGUSR1 只允许 mic_pump 线程处理（它会自己 UNBLOCK）；其余线程屏蔽掉，
     * 避免误触发。主线程以及之后 pthread_create 出来的线程都继承此 mask。 */
    {
        sigset_t s; sigemptyset(&s); sigaddset(&s, SIGUSR1);
        pthread_sigmask(SIG_BLOCK, &s, NULL);
    }

    printf("[my-app] booting (LVGL UI + camera capture)\n");

    if (event_bus_init() != 0) {
        fprintf(stderr, "event_bus_init failed\n");
        return 1;
    }

    if (waveguide_init() != 0) {
        fprintf(stderr, "waveguide_init failed\n");
        return 1;
    }
    waveguide_clear();
    waveguide_set_brightness(128);

    if (lvgl_setup() != 0) {
        fprintf(stderr, "lvgl_setup failed\n");
        return 1;
    }
    ui_init();
    show_home();

    /* --- 构建功能页附加控件，并完成 lumina 子应用绑定 --- */
    build_page_widgets(ui_SceneWordsContainer,      &g_vlm_w, /*crop*/1, /*tree*/0);
    build_page_widgets(ui_EnglishTalkContainer,     &g_tr_w,  0, 0);
    build_page_widgets(ui_PhotoSearchContainer,     &g_hw_w,  0, 1);
    build_page_widgets(ui_EnglishPracticeContainer, &g_ep_w,  0, 0);

    app_common_bind_lock(ui_lock_wrap, ui_unlock_wrap);

    /* 左上角常驻收音指示器（放在 top 层，任何容器切换都可见） */
    {
        lv_obj_t *mic_lbl = lv_label_create(lv_layer_top());
        lv_obj_set_pos(mic_lbl, 10, 6);
        lv_obj_set_style_text_font(mic_lbl, &ui_font_alibaba_30, 0);
        lv_obj_set_style_text_color(mic_lbl, lv_color_make(150, 150, 150), 0);
        lv_label_set_text(mic_lbl, "收音:关");
        app_common_set_mic_indicator(mic_lbl);
    }

    myapp_config_t cfg;
    myapp_config_load(&cfg);
    app_vlm_configure      (cfg.server_host, cfg.vlm_port);
    app_translate_configure(cfg.server_host, cfg.translate_port);
    app_homework_configure (cfg.server_host, cfg.homework_port);

    /* 用 designated initializer，避免字段顺序变化时悄悄错位 */
    g_apps[HOME_SCENE]  = (app_entry_t){
        .enter    = app_vlm_enter,
        .exit     = app_vlm_exit,
        .confirm  = app_vlm_on_confirm,
        .t1_short = NULL,
        .t1_long  = NULL,
        .widgets  = &g_vlm_w,
        .container= ui_SceneWordsContainer,
        .title    = "多模态百科" };
    g_apps[HOME_TALK]   = (app_entry_t){
        .enter    = app_translate_enter,
        .exit     = app_translate_exit,
        .confirm  = app_translate_on_confirm,
        .t1_short = NULL,
        .t1_long  = NULL,
        .widgets  = &g_tr_w,
        .container= ui_EnglishTalkContainer,
        .title    = "拟境英语" };
    /* 拍照搜题专属：T1 短按=光标下移，T1 长按=展开当前节点 */
    g_apps[HOME_SEARCH] = (app_entry_t){
        .enter    = app_homework_enter,
        .exit     = app_homework_exit,
        .confirm  = app_homework_on_confirm,
        .t1_short = app_homework_on_nav_down,
        .t1_long  = app_homework_on_act_expand,
        .widgets  = &g_hw_w,
        .container= ui_PhotoSearchContainer,
        .title    = "拍照搜题" };
    /* 英语对练：后端待接入。enter/exit/confirm 全为 NULL，
     * enter_function 进入时只会切页面，不会调用任何业务回调，
     * 因此不会崩；install_app_ui 使用 g_ep_w（空 label）。 */
    g_apps[HOME_ENGLISH] = (app_entry_t){
        .enter    = NULL,
        .exit     = NULL,
        .confirm  = NULL,
        .t1_short = NULL,
        .t1_long  = NULL,
        .widgets  = &g_ep_w,
        .container= ui_EnglishPracticeContainer,
        .title    = "英语对练" };

    if (gpio_hub_init() == 0) {
        gpio_hub_add(GPIO_ACT, GPIO_ACTIVE_LOW);
        gpio_hub_add(GPIO_NAV, GPIO_ACTIVE_LOW);
        /* press 事件只给电源键用；触摸板 1/2 的真正动作在 release 里按
         * press_duration_ms 判定短按 / 长按。 */
        event_bus_subscribe("gpio/press",   on_gpio_press,   NULL);
        event_bus_subscribe("gpio/release", on_gpio_release, NULL);
    }

    /* 物理电源键（KEY_POWER via /dev/input）→ 作为"收音 toggle"键 */
    if (power_key_init() != 0) {
        fprintf(stderr, "[my-app] power_key_init failed (非致命)\n");
    }

    audio_player_init();
    tts_player_init();

    if (wifi_init(cfg.wifi_iface) == 0) {
        wifi_connect(cfg.wifi_ssid, cfg.wifi_password);
    }

    printf("[my-app] entering LVGL main loop\n");

    g_lp_running = 1;
    pthread_create(&g_lp_thread, NULL, longpress_thread_main, NULL);

    while (g_running) {
        pthread_mutex_lock(&g_ui_mutex);
        lv_timer_handler();
        pthread_mutex_unlock(&g_ui_mutex);
        lv_tick_inc(5);
        usleep(5 * 1000);
    }

    g_lp_running = 0;
    pthread_join(g_lp_thread, NULL);

    printf("[my-app] shutting down\n");
    wifi_shutdown();
    power_key_shutdown();
    gpio_hub_shutdown();
    tts_player_shutdown();
    audio_player_shutdown();
    waveguide_shutdown();
    event_bus_shutdown();
    return 0;
}

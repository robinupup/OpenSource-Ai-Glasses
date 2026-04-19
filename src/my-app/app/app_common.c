#include "app_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <setjmp.h>
#include <jpeglib.h>

#include "lvgl/lvgl.h"

static app_ui_t g_ui;
static lock_fn_t g_lock = NULL;
static lock_fn_t g_unlock = NULL;
static lv_obj_t *g_mic_indicator = NULL;

void app_common_set_ui(const app_ui_t *ui) {
    if (!ui) { memset(&g_ui, 0, sizeof(g_ui)); return; }
    g_ui = *ui;
}
const app_ui_t *app_common_get_ui(void) { return &g_ui; }

void app_common_bind_lock(lock_fn_t lock, lock_fn_t unlock) {
    g_lock = lock;
    g_unlock = unlock;
}
void app_ui_lock(void)   { if (g_lock)   g_lock();   }
void app_ui_unlock(void) { if (g_unlock) g_unlock(); }

void app_ui_set_text(lv_obj_t *label, const char *text) {
    if (!label) return;
    app_ui_lock();
    lv_label_set_text(label, text ? text : "");
    app_ui_unlock();
}

void app_ui_append_text(lv_obj_t *label, const char *piece) {
    if (!label || !piece || !*piece) return;
    app_ui_lock();
    lv_label_ins_text(label, LV_LABEL_POS_LAST, piece);
    app_ui_unlock();
}

void app_ui_clear_text(lv_obj_t *label) {
    if (!label) return;
    app_ui_lock();
    lv_label_set_text(label, "");
    app_ui_unlock();
}

void app_ui_show_asr(const char *text) {
    lv_obj_t *lbl = g_ui.asr_label;
    if (!lbl) return;
    app_ui_lock();
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lbl, text ? text : "");
    app_ui_unlock();
}

void app_ui_hide_asr(void) {
    lv_obj_t *lbl = g_ui.asr_label;
    if (!lbl) return;
    app_ui_lock();
    lv_label_set_text(lbl, "");
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    app_ui_unlock();
}

/* ---- base64 decode（只支持标准字母表，忽略空白/换行） ---- */
static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static uint8_t *base64_decode_str(const char *b64, size_t *out_len) {
    if (!b64) return NULL;
    size_t len = strlen(b64);
    uint8_t *out = (uint8_t *)malloc((len / 4 + 4) * 3);
    if (!out) return NULL;
    size_t o = 0; int g[4]; int gc = 0;
    for (size_t i = 0; i < len; i++) {
        char c = b64[i];
        if (c == '=') break;
        int v = b64_val((unsigned char)c);
        if (v < 0) continue;
        g[gc++] = v;
        if (gc == 4) {
            out[o++] = (g[0] << 2) | (g[1] >> 4);
            out[o++] = ((g[1] & 0xF) << 4) | (g[2] >> 2);
            out[o++] = ((g[2] & 0x3) << 6) | g[3];
            gc = 0;
        }
    }
    if (gc >= 2) {
        out[o++] = (g[0] << 2) | (g[1] >> 4);
        if (gc >= 3) out[o++] = ((g[1] & 0xF) << 4) | (g[2] >> 2);
    }
    *out_len = o;
    return out;
}

/* ---- JPEG 解码 → LV_COLOR_DEPTH=8（RGB332 + alpha）像素缓冲 ---- */
struct jerr_ctx { struct jpeg_error_mgr base; jmp_buf jmp; };
static void jerr_exit(j_common_ptr cinfo) {
    struct jerr_ctx *e = (struct jerr_ctx *)cinfo->err;
    longjmp(e->jmp, 1);
}
static void jerr_noop(j_common_ptr c) { (void)c; }

static int jpeg_decode_to_lvgl8(const uint8_t *jpg, size_t jpg_len,
                                int target_max, uint8_t **out_buf,
                                int *out_w, int *out_h) {
    struct jpeg_decompress_struct cinfo;
    struct jerr_ctx jerr;
    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.err = jpeg_std_error(&jerr.base);
    jerr.base.error_exit = jerr_exit;
    jerr.base.output_message = jerr_noop;
    if (setjmp(jerr.jmp)) { jpeg_destroy_decompress(&cinfo); return -1; }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char *)jpg, jpg_len);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_GRAYSCALE;
    /* 选 1/1, 1/2, 1/4, 1/8 中使最长边 <= target_max 的最大尺寸 */
    int denom = 1;
    int maxdim = (int)(cinfo.image_width > cinfo.image_height ?
                       cinfo.image_width : cinfo.image_height);
    while (denom < 8 && maxdim / (denom * 2) >= target_max) denom *= 2;
    cinfo.scale_num = 1;
    cinfo.scale_denom = denom;
    jpeg_start_decompress(&cinfo);
    int w = (int)cinfo.output_width;
    int h = (int)cinfo.output_height;
    size_t buf_sz = (size_t)w * h * 2;
    uint8_t *buf = (uint8_t *)malloc(buf_sz);
    uint8_t *row = (uint8_t *)malloc((size_t)w);
    if (!buf || !row) { free(buf); free(row); jpeg_destroy_decompress(&cinfo); return -1; }
    uint8_t *dst = buf;
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW rr[1] = { row };
        jpeg_read_scanlines(&cinfo, rr, 1);
        for (int x = 0; x < w; x++) {
            uint8_t y = row[x];
            /* RGB332：R=Y>>5, G=Y>>5, B=Y>>6 */
            *dst++ = ((y & 0xE0)) | ((y & 0xE0) >> 3) | (y >> 6);
            *dst++ = 0xFF;
        }
    }
    free(row);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    *out_buf = buf; *out_w = w; *out_h = h;
    return 0;
}

/* 持久化保存最后一帧，供 lv_img 引用 */
static lv_img_dsc_t g_crop_dsc = {0};
static uint8_t     *g_crop_buf = NULL;

void app_ui_clear_crop(void) {
    const app_ui_t *ui = app_common_get_ui();
    if (!ui->crop_img) return;
    app_ui_lock();
    lv_img_set_src(ui->crop_img, NULL);
    lv_img_cache_invalidate_src(&g_crop_dsc);
    if (g_crop_buf) { free(g_crop_buf); g_crop_buf = NULL; }
    memset(&g_crop_dsc, 0, sizeof(g_crop_dsc));
    app_ui_unlock();
}

void app_ui_show_crop_b64_jpeg(const char *b64) {
    if (!b64 || !*b64) return;
    const app_ui_t *ui = app_common_get_ui();
    if (!ui->crop_img) return;

    /* 兼容 data URL 头 */
    const char *p = strstr(b64, "base64,");
    if (p) b64 = p + 7;

    size_t jpg_len = 0;
    uint8_t *jpg = base64_decode_str(b64, &jpg_len);
    if (!jpg || jpg_len < 16) { free(jpg); return; }

    uint8_t *pix = NULL; int w = 0, h = 0;
    int rc = jpeg_decode_to_lvgl8(jpg, jpg_len, 200, &pix, &w, &h);
    free(jpg);
    if (rc != 0 || !pix) return;

    app_ui_lock();
    if (g_crop_buf) { free(g_crop_buf); g_crop_buf = NULL; }
    g_crop_buf = pix;
    g_crop_dsc.header.always_zero = 0;
    g_crop_dsc.header.w  = w;
    g_crop_dsc.header.h  = h;
    g_crop_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    g_crop_dsc.data_size = (size_t)w * h * 2;
    g_crop_dsc.data      = pix;
    lv_img_cache_invalidate_src(&g_crop_dsc);
    lv_img_set_src(ui->crop_img, &g_crop_dsc);
    lv_obj_set_size(ui->crop_img, w, h);
    /* 图片紧贴 content 文字下方居中显示（间隙=0 真正贴齐） */
    if (ui->content_label) {
        lv_obj_align_to(ui->crop_img, ui->content_label,
                        LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    }
    app_ui_unlock();
}

void app_ui_set_tree_layer1(const char *n0, const char *n1, const char *n2) {
    const char *nodes[3] = { n0, n1, n2 };
    for (int i = 0; i < 3; i++) {
        if (g_ui.tree_labels[i]) app_ui_set_text(g_ui.tree_labels[i], nodes[i] ? nodes[i] : "");
    }
}

void app_common_set_mic_indicator(lv_obj_t *label) {
    g_mic_indicator = label;
    app_common_set_mic_on(0);
}

void app_common_set_mic_on(int on) {
    if (!g_mic_indicator) return;
    app_ui_lock();
    lv_label_set_text(g_mic_indicator, on ? "收音：开" : "收音：关");
    lv_obj_set_style_text_color(g_mic_indicator,
        on ? lv_color_make(255, 96, 96) : lv_color_make(150, 150, 150), 0);
    app_ui_unlock();
}

void app_gen_uuid(char *out, size_t out_len) {
    if (!out || out_len < 37) { if (out && out_len) out[0] = '\0'; return; }
    FILE *fp = fopen("/proc/sys/kernel/random/uuid", "r");
    if (fp) {
        size_t n = fread(out, 1, out_len - 1, fp);
        fclose(fp);
        if (n > 0) {
            out[n] = '\0';
            while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r' || out[n-1] == ' ')) {
                out[--n] = '\0';
            }
            if (n > 0) return;
        }
    }
    /* 回退：时间戳+随机 */
    unsigned int seed = (unsigned int)(time(NULL) ^ (unsigned long)getpid());
    srand(seed);
    snprintf(out, out_len, "%08x-%04x-%04x-%04x-%012x",
             (unsigned int)time(NULL),
             rand() & 0xFFFF, rand() & 0xFFFF,
             rand() & 0xFFFF, (unsigned int)rand());
}

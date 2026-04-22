/*
 * wb_compare.c — 读 NV12 raw，用与 main.c::nv12_to_jpeg 完全相同的路径
 * 分别生成 "关 AWB" 和 "开 AWB" 两张 JPEG，并打印 Cb/Cr 均值对比。
 *
 * 编译：
 *   $TC tools/wb_compare.c -O2 -o build/wb_compare -ljpeg
 * 运行：
 *   /tmp/wb_compare /tmp/snap.nv12 /tmp/before.jpg /tmp/after.jpg
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <jpeglib.h>

#define W 1920
#define H 1080

static int nv12_to_jpeg(const uint8_t *y, const uint8_t *uv,
                        const char *jpeg_path, int w, int h,
                        int quality, int awb_on,
                        int *out_avg_cb, int *out_avg_cr) {
    int awb_dv = 0, awb_du = 0;
    uint64_t sV = 0, sU = 0;
    size_t pairs = (size_t)w * h / 4;
    for (size_t i = 0; i < pairs; i++) {
        sV += uv[2 * i + 0];
        sU += uv[2 * i + 1];
    }
    int avg_v = (int)(sV / pairs);
    int avg_u = (int)(sU / pairs);
    if (awb_on) {
        awb_dv = avg_v - 128;
        awb_du = avg_u - 128;
        if (awb_dv >  32) awb_dv =  32;
        if (awb_dv < -32) awb_dv = -32;
        if (awb_du >  32) awb_du =  32;
        if (awb_du < -32) awb_du = -32;
    }

    FILE *out = fopen(jpeg_path, "wb");
    if (!out) { perror(jpeg_path); return -1; }

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

    uint8_t *row = (uint8_t *)malloc((size_t)w * 3);

    /* 同时统计修正后的 Cb/Cr 均值 */
    uint64_t s_cb = 0, s_cr = 0;
    size_t cnt = 0;

    while (cinfo.next_scanline < (JDIMENSION)h) {
        int line = cinfo.next_scanline;
        const uint8_t *yrow  = y  + line * w;
        const uint8_t *uvrow = uv + (line / 2) * w;
        uint8_t *dst = row;
        for (int x = 0; x < w; x++) {
            int uv_idx = (x / 2) * 2;
            int cb = (int)uvrow[uv_idx + 1] - awb_du;
            int cr = (int)uvrow[uv_idx + 0] - awb_dv;
            if (cb < 0) cb = 0; else if (cb > 255) cb = 255;
            if (cr < 0) cr = 0; else if (cr > 255) cr = 255;
            *dst++ = yrow[x];
            *dst++ = (uint8_t)cb;
            *dst++ = (uint8_t)cr;
            s_cb += cb; s_cr += cr; cnt++;
        }
        JSAMPROW r = row;
        jpeg_write_scanlines(&cinfo, &r, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(out);
    free(row);

    *out_avg_cb = (int)(s_cb / cnt);
    *out_avg_cr = (int)(s_cr / cnt);
    printf("[wb] %s: awb=%s du=%+d dv=%+d  → out Cb=%d Cr=%d\n",
           jpeg_path, awb_on ? "ON " : "OFF",
           -awb_du, -awb_dv, *out_avg_cb, *out_avg_cr);
    return 0;
}

int main(int argc, char **argv) {
    const char *in   = argc >= 2 ? argv[1] : "/tmp/snap.nv12";
    const char *j0   = argc >= 3 ? argv[2] : "/tmp/before.jpg";
    const char *j1   = argc >= 4 ? argv[3] : "/tmp/after.jpg";

    size_t ysize  = (size_t)W * H;
    size_t uvsize = ysize / 2;
    uint8_t *y  = malloc(ysize);
    uint8_t *uv = malloc(uvsize);
    FILE *fp = fopen(in, "rb");
    if (!fp) { perror(in); return 1; }
    if (fread(y, 1, ysize, fp) != ysize ||
        fread(uv, 1, uvsize, fp) != uvsize) {
        fprintf(stderr, "input too short\n"); return 1;
    }
    fclose(fp);

    int cb0, cr0, cb1, cr1;
    nv12_to_jpeg(y, uv, j0, W, H, 90, 0, &cb0, &cr0);
    nv12_to_jpeg(y, uv, j1, W, H, 90, 1, &cb1, &cr1);

    printf("[wb] summary:\n");
    printf("       BEFORE  Cb=%d (Δ=%+d)  Cr=%d (Δ=%+d)\n",
           cb0, cb0 - 128, cr0, cr0 - 128);
    printf("       AFTER   Cb=%d (Δ=%+d)  Cr=%d (Δ=%+d)\n",
           cb1, cb1 - 128, cr1, cr1 - 128);
    free(y); free(uv);
    return 0;
}

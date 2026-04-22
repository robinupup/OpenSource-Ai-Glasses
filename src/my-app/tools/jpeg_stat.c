/* jpeg_stat.c —— 解 JPEG，打印 avg R/G/B，看 AWB 效果。
 * 编译时链接 libjpeg：$TC jpeg_stat.c -ljpeg -o jpeg_stat */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <jpeglib.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file.jpg\n", argv[0]); return 1; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 1; }

    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int w = cinfo.output_width, h = cinfo.output_height;
    int stride = w * 3;
    uint8_t *row = malloc(stride);
    uint64_t sR=0,sG=0,sB=0;
    while (cinfo.output_scanline < (JDIMENSION)h) {
        JSAMPROW r = row;
        jpeg_read_scanlines(&cinfo, &r, 1);
        for (int x=0; x<w; x++) {
            sR += row[x*3+0]; sG += row[x*3+1]; sB += row[x*3+2];
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    free(row);

    double n = (double)w * h;
    double aR=sR/n, aG=sG/n, aB=sB/n;
    printf("%s  %dx%d  avg R=%.1f G=%.1f B=%.1f  ratio G/R=%.3f G/B=%.3f\n",
           argv[1], w, h, aR, aG, aB, aG/aR, aG/aB);
    return 0;
}

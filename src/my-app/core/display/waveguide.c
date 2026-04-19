#include "waveguide.h"
#include "hal_driver.h"
#include "jbd013_api.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int g_initialized = 0;

int waveguide_init(void) {
    if (g_initialized) return 0;
    if (spi_init() != 0) {
        fprintf(stderr, "[waveguide] spi_init failed\n");
        return -1;
    }
    panel_init();
    send_cmd(SPI_DISPLAY_ENABLE);
    g_initialized = 1;
    printf("[waveguide] panel ready (%dx%d, 4bpp)\n",
           WAVEGUIDE_WIDTH, WAVEGUIDE_HEIGHT);
    return 0;
}

void waveguide_shutdown(void) {
    if (!g_initialized) return;
    send_cmd(SPI_DISPLAY_DISABLE);
    send_cmd(SPI_DEEP_POWER_DOWN);
    g_initialized = 0;
}

int waveguide_enable(int on) {
    if (!g_initialized) return -1;
    send_cmd(on ? SPI_DISPLAY_ENABLE : SPI_DISPLAY_DISABLE);
    return 0;
}

int waveguide_set_brightness(int value) {
    /*
     * JBD013 亮度寄存器范围与刷新率相关:
     *   25Hz  -> 0..21331
     *   50Hz  -> 0..10664
     *   75Hz  -> 0..7109
     *   100Hz -> 0..5331
     *   150Hz -> 0..3366
     * 典型工作值 3000 (display-service panel_init 默认值)。
     * 之前错误地 clamp 到 0..255 会导致屏幕显示极暗/发白。
     */
    if (!g_initialized) return -1;
    if (value < 0)     value = 0;
    if (value > 10664) value = 10664;
    wr_lum_reg((uint16_t)value);
    return 0;
}

int waveguide_set_mirror(int mask) {
    if (!g_initialized) return -1;
    set_mirror_mode((uint8_t)(mask & 0x03));
    return 0;
}

int waveguide_commit_framebuffer(const uint8_t *buf) {
    if (!g_initialized || !buf) return -1;
    int rc = spi_wr_buffer(0, 0, (uint8_t *)buf, WAVEGUIDE_FRAME_SIZE);
    /* 关键：必须发 SPI_SYNC 把写入的数据从缓存同步到显示 RAM，否则屏上看不到。
     * 与 display-service/jbd013_api.c 的 display_image() 行为保持一致。 */
    send_cmd(SPI_SYNC);
    usleep(1 * 1000);
    return rc;
}

int waveguide_commit_region(int x, int y, int w, int h, const uint8_t *buf) {
    if (!g_initialized || !buf) return -1;
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return -1;
    if ((x & 1) || (w & 1)) return -1; /* 4bpp 必须偶像素对齐 */
    if (x + w > WAVEGUIDE_WIDTH || y + h > WAVEGUIDE_HEIGHT) return -1;

    int rc = 0;
    /* 对于非整行的区域，必须一行一行写入（列不连续） */
    if (w == WAVEGUIDE_WIDTH) {
        rc = spi_wr_buffer(0, (uint16_t)y, (uint8_t *)buf, (uint32_t)(w * h / 2));
    } else {
        uint32_t stride_bytes = (uint32_t)(w / 2);
        for (int row = 0; row < h; row++) {
            rc = spi_wr_buffer((uint16_t)x, (uint16_t)(y + row),
                               (uint8_t *)(buf + row * stride_bytes),
                               stride_bytes);
            if (rc != 0) return rc;
        }
    }
    send_cmd(SPI_SYNC);
    usleep(1 * 1000);
    return rc;
}

int waveguide_clear(void) {
    if (!g_initialized) return -1;
    static uint8_t zero[WAVEGUIDE_FRAME_SIZE];
    memset(zero, 0, sizeof(zero));
    int rc = spi_wr_buffer(0, 0, zero, WAVEGUIDE_FRAME_SIZE);
    send_cmd(SPI_SYNC);
    usleep(1 * 1000);
    return rc;
}

float waveguide_get_temperature(void) {
    if (!g_initialized) return 0.0f;
    return get_temperature_sensor_data();
}

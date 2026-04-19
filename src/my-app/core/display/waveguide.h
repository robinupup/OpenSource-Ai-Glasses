/**
 * my-app core - 光波导显示接口
 *
 * 直接驱动 JBD013 单色 OLED 微显示屏（通过 /dev/spidev0.0）。
 * 输出格式：4bpp 灰度，每字节 2 像素（高 4 位 = 左像素，低 4 位 = 右像素）。
 *
 * 分辨率：640 x 480
 * 帧缓存：640*480/2 = 153600 字节
 *
 * 接口简介：
 *   waveguide_init();                        // 打开 SPI，初始化面板
 *   waveguide_enable(1);                     // 打开显示
 *   waveguide_set_brightness(0..255);        // 调亮度
 *   waveguide_commit_framebuffer(buf);       // 整屏刷新
 *   waveguide_commit_region(x,y,w,h,buf);    // 局部刷新
 *   waveguide_clear();                       // 清屏
 *   waveguide_shutdown();                    // 关面板、关 SPI
 */

#ifndef MYAPP_WAVEGUIDE_H
#define MYAPP_WAVEGUIDE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAVEGUIDE_WIDTH        640
#define WAVEGUIDE_HEIGHT       480
#define WAVEGUIDE_BPP          4
#define WAVEGUIDE_FRAME_SIZE   (WAVEGUIDE_WIDTH * WAVEGUIDE_HEIGHT / 2)

/** 打开 SPI 并初始化面板。 */
int  waveguide_init(void);

/** 关闭 */
void waveguide_shutdown(void);

/** 打开/关闭显示 */
int  waveguide_enable(int on);

/** 亮度：0..255，底层会写到 JBD013 的亮度寄存器 */
int  waveguide_set_brightness(int value);

/** 镜像翻转：bit0 = 左右, bit1 = 上下 */
int  waveguide_set_mirror(int mask);

/** 整屏提交：buf 必须 >= WAVEGUIDE_FRAME_SIZE 字节 */
int  waveguide_commit_framebuffer(const uint8_t *buf);

/** 局部提交。x/w 必须偶数（4bpp 双像素字节对齐）。 */
int  waveguide_commit_region(int x, int y, int w, int h, const uint8_t *buf);

/** 清屏为 0x00（黑） */
int  waveguide_clear(void);

/** 读面板温度 */
float waveguide_get_temperature(void);

#ifdef __cplusplus
}
#endif

#endif

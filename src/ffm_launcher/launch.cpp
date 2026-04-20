#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <cstdarg> 
#include <time.h> // 引入时间相关头文件用于日志
#include <stdint.h> // 引入 uint32_t 等类型
#include <sys/wait.h> // 用于 system() 的返回值检查
#include <signal.h> // 信号处理

#include <ctype.h>
#include "../../SDK/ai_glass_sdk/include/ai_display.h"

// ================================================================
// BLE 文本显示模块
// 协议：docs/api_docs/01_蓝牙接口文档.md
//   手机 GATT Write (0x8101) → btgatt-server → "BLE:XX XX XX..."
//   → 共享内存 /display_shm → 本进程解析 UTF-8 → 眼镜屏幕显示
// ================================================================

// 8x8 单色位图字体（可打印 ASCII 0x20–0x7E，共 95 个字符）
// 每行 1 字节，bit7=最左像素；0=黑，0xF=白
static const uint8_t g_font8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 0x20 ' '
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // 0x21 '!'
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, // 0x22 '"'
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // 0x23 '#'
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // 0x24 '$'
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // 0x25 '%'
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // 0x26 '&'
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, // 0x27 '\''
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, // 0x28 '('
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, // 0x29 ')'
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // 0x2A '*'
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // 0x2B '+'
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, // 0x2C ','
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // 0x2D '-'
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // 0x2E '.'
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // 0x2F '/'
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // 0x30 '0'
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // 0x31 '1'
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // 0x32 '2'
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // 0x33 '3'
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // 0x34 '4'
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // 0x35 '5'
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // 0x36 '6'
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // 0x37 '7'
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // 0x38 '8'
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // 0x39 '9'
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, // 0x3A ':'
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, // 0x3B ';'
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, // 0x3C '<'
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, // 0x3D '='
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // 0x3E '>'
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // 0x3F '?'
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // 0x40 '@'
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // 0x41 'A'
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // 0x42 'B'
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // 0x43 'C'
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // 0x44 'D'
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // 0x45 'E'
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // 0x46 'F'
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // 0x47 'G'
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // 0x48 'H'
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 0x49 'I'
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // 0x4A 'J'
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // 0x4B 'K'
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // 0x4C 'L'
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // 0x4D 'M'
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, // 0x4E 'N'
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // 0x4F 'O'
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // 0x50 'P'
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // 0x51 'Q'
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // 0x52 'R'
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // 0x53 'S'
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 0x54 'T'
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // 0x55 'U'
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 0x56 'V'
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 0x57 'W'
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, // 0x58 'X'
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // 0x59 'Y'
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // 0x5A 'Z'
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, // 0x5B '['
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // 0x5C backslash
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // 0x5D ']'
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // 0x5E '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // 0x5F '_'
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // 0x60 '`'
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, // 0x61 'a'
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, // 0x62 'b'
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, // 0x63 'c'
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00}, // 0x64 'd'
    {0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00}, // 0x65 'e'
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0x00}, // 0x66 'f'
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, // 0x67 'g'
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, // 0x68 'h'
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, // 0x69 'i'
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, // 0x6A 'j'
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, // 0x6B 'k'
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 0x6C 'l'
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, // 0x6D 'm'
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, // 0x6E 'n'
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, // 0x6F 'o'
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, // 0x70 'p'
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, // 0x71 'q'
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, // 0x72 'r'
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, // 0x73 's'
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, // 0x74 't'
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, // 0x75 'u'
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 0x76 'v'
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, // 0x77 'w'
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, // 0x78 'x'
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, // 0x79 'y'
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, // 0x7A 'z'
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, // 0x7B '{'
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // 0x7C '|'
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, // 0x7D '}'
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, // 0x7E '~'
};

// 显示客户端句柄（全局持久化，避免每次显示重复初始化）
static ai_display_client_t *g_disp_client = NULL;

// 初始化显示客户端（在 main() 最开始调用一次）
static int init_display_client(void) {
    g_disp_client = ai_display_init();
    if (!g_disp_client) {
        fprintf(stderr, "[LAUNCH] ai_display_init failed\n");
        return -1;
    }
    int ret = ai_display_connect(g_disp_client);
    if (ret != AI_DISPLAY_SUCCESS) {
        fprintf(stderr, "[LAUNCH] ai_display_connect failed: %s\n",
                ai_display_get_error_string(ret));
        ai_display_cleanup(g_disp_client);
        g_disp_client = NULL;
        return -1;
    }
    ai_display_request_focus(g_disp_client);
    printf("[LAUNCH] Display client connected OK\n");
    return 0;
}

// 在 4bpp 帧缓冲中设置单个像素（灰度 0~15）
// 640px/行 → 每行 320 字节；高4位=偶数列像素，低4位=奇数列像素
static void fb_set_pixel(uint8_t *fb, int x, int y, uint8_t gray) {
    if (x < 0 || x >= AI_DISPLAY_WIDTH || y < 0 || y >= AI_DISPLAY_HEIGHT) return;
    int off = y * (AI_DISPLAY_WIDTH / 2) + x / 2;
    if (x % 2 == 0)
        fb[off] = (uint8_t)((gray << 4) | (fb[off] & 0x0F));
    else
        fb[off] = (uint8_t)((fb[off] & 0xF0) | (gray & 0x0F));
}

// 在帧缓冲中绘制单个 ASCII 字符（scale 倍放大）
static void fb_draw_char(uint8_t *fb, int x, int y, char c, int scale) {
    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) c = '?';
    const uint8_t *glyph = g_font8x8[(unsigned char)c - 0x20];
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            uint8_t pix = (glyph[row] & (0x80u >> col)) ? 0xF : 0x0;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    fb_set_pixel(fb, x + col*scale + sx, y + row*scale + sy, pix);
        }
    }
}

// 将 "BLE:XX XX XX ..." 中的十六进制字节解码回原始 UTF-8 字节串
// 返回解码字节数，-1 表示格式错误
static int parse_ble_hex_to_bytes(const char *ble_msg, uint8_t *out, int max_len) {
    if (strncmp(ble_msg, "BLE:", 4) != 0) return -1;
    const char *p = ble_msg + 4;
    int count = 0;
    while (*p && count < max_len - 1) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1])) {
            uint8_t hi = (uint8_t)(isdigit((unsigned char)p[0])
                         ? p[0]-'0' : toupper((unsigned char)p[0])-'A'+10);
            uint8_t lo = (uint8_t)(isdigit((unsigned char)p[1])
                         ? p[1]-'0' : toupper((unsigned char)p[1])-'A'+10);
            out[count++] = (uint8_t)((hi << 4) | lo);
            p += 2;
        } else {
            break;
        }
    }
    out[count] = '\0';
    return count;
}

// 将解码后的 UTF-8 文本渲染到眼镜屏幕
// 非 ASCII 的多字节字符（如汉字）显示为方块"□"占位
static void display_text_on_glasses(const char *utf8_text) {
    if (!g_disp_client) {
        fprintf(stderr, "[LAUNCH] Display client not available\n");
        return;
    }
    uint8_t *fb = ai_display_get_framebuffer(g_disp_client);
    if (!fb) {
        fprintf(stderr, "[LAUNCH] Failed to get framebuffer\n");
        return;
    }

    // 清屏（全黑）
    memset(fb, 0x00, AI_DISPLAY_FRAME_SIZE);

    const int SCALE    = 2;          // 字符放大倍数：每字符 16x16px
    const int CW       = 8 * SCALE;  // 字符宽度 16px
    const int CH       = 8 * SCALE;  // 字符高度 16px
    const int MARGIN_X = 8;          // 左边距
    const int MARGIN_Y = 16;         // 上边距
    const int LINE_GAP = 4;          // 行间距
    const int MAX_X    = AI_DISPLAY_WIDTH  - MARGIN_X;
    const int MAX_Y    = AI_DISPLAY_HEIGHT - CH;

    int cx = MARGIN_X, cy = MARGIN_Y;
    const unsigned char *p = (const unsigned char *)utf8_text;

    while (*p && cy <= MAX_Y) {
        unsigned char byte = *p;

        if (byte == '\n' || byte == '\r') {
            cx  = MARGIN_X;
            cy += CH + LINE_GAP;
            p++;
            continue;
        }

        // 计算 UTF-8 字节数
        int char_bytes = 1;
        if      ((byte & 0xE0) == 0xC0) char_bytes = 2;
        else if ((byte & 0xF0) == 0xE0) char_bytes = 3;
        else if ((byte & 0xF8) == 0xF0) char_bytes = 4;

        // 自动换行
        if (cx + CW > MAX_X) {
            cx  = MARGIN_X;
            cy += CH + LINE_GAP;
        }
        if (cy > MAX_Y) break;

        if (char_bytes == 1 && byte >= 0x20) {
            fb_draw_char(fb, cx, cy, (char)byte, SCALE);
        } else {
            // 汉字等多字节字符：显示 '[' 占位
            fb_draw_char(fb, cx, cy, '[', SCALE);
        }
        cx += CW + 1;
        p  += char_bytes;
    }

    int ret = ai_display_commit_frame(g_disp_client, 0, 0, 0,
                                      AI_DISPLAY_WIDTH, AI_DISPLAY_HEIGHT);
    if (ret != AI_DISPLAY_SUCCESS)
        fprintf(stderr, "[LAUNCH] commit_frame failed: %s\n", ai_display_get_error_string(ret));
    else
        printf("[LAUNCH] Text rendered on glasses: %s\n", utf8_text);
}

// 统一处理所有 BLE 消息：拍照指令 or 显示文字
static void handle_ble_message(const char *ble_msg) {
    // "LAUNCH\n" = 4C 41 55 4E 43 48 0A → 触发拍照
    if (strncmp(ble_msg, "BLE:4C 41 55 4E 43 48 0A", 24) == 0) {
        log_info("BLE: LAUNCH command → starting capture");
        process_capture();
        return;
    }
    // 其他：解码为 UTF-8 文本并显示到眼镜屏幕
    uint8_t text_buf[256];
    int len = parse_ble_hex_to_bytes(ble_msg, text_buf, (int)sizeof(text_buf));
    if (len <= 0) {
        log_info("BLE: cannot parse hex message: %s", ble_msg);
        return;
    }
    log_info("BLE: decoded %d bytes text: \"%s\"", len, (char *)text_buf);
    display_text_on_glasses((const char *)text_buf);
}


// --- Camera Config ---
#define DEVICE "/dev/video7"
#define WIDTH 1920
#define HEIGHT 1080
// ---------------------

// --- IPC Config (与 display/main.c 保持一致) ---
#define SHM_NAME "/display_shm" // 共享内存名称
#define SEM_NAME "/display_sem" // 信号量名称
#define BUFFER_SIZE 128         // 消息缓冲区大小
// -----------------------------------------------

// --- 日志宏定义 (简化版，与 display/main.c 风格类似) ---
#define LOG_TAG "[LAUNCH]"
void log_info(const char *format, ...) {
    time_t now;
    struct tm *tm_info;
    char buffer[26];
    time(&now);
    tm_info = localtime(&now);
    strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
    printf("[%s] %s [INFO] ", buffer, LOG_TAG);
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

void log_error(const char *format, ...) {
    time_t now;
    struct tm *tm_info;
    char buffer[26];
    time(&now);
    tm_info = localtime(&now);
    strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
    fprintf(stderr, "[%s] %s [ERROR] ", buffer, LOG_TAG);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
    fflush(stderr);
}

void log_debug(const char *format, ...) {
#ifdef DEBUG
    time_t now;
    struct tm *tm_info;
    char buffer[26];
    time(&now);
    tm_info = localtime(&now);
    strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
    printf("[%s] %s [DEBUG] ", buffer, LOG_TAG);
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
#endif
}
// --------------------------------------------------------

// 全局变量用于 cleanup (与 display/main.c 保持一致)
static int shm_fd = -1;
static void *shared_memory = MAP_FAILED;
static sem_t *semaphore = SEM_FAILED;
static volatile sig_atomic_t running = 1; // 用于信号处理

// 信号处理函数 (与 display/main.c 保持一致)
void cleanup(int sig) {
    log_info("Received signal %d, cleaning up...", sig);
    running = 0; // 设置运行标志为假

    // 清理共享内存映射
    if (shared_memory != MAP_FAILED) {
        if (munmap(shared_memory, BUFFER_SIZE) == -1) {
            log_error("Failed to unmap shared memory: %s", strerror(errno));
        } else {
            log_debug("Shared memory unmapped.");
        }
        shared_memory = MAP_FAILED;
    }

    // 关闭共享内存文件描述符 (注意：display中未明确close shm_fd，但最好加上)
    if (shm_fd != -1) {
        close(shm_fd);
        shm_fd = -1;
        log_debug("Shared memory file descriptor closed.");
    }

    // 关闭信号量
    if (semaphore != SEM_FAILED) {
        if (sem_close(semaphore) == -1) {
            log_error("Failed to close semaphore: %s", strerror(errno));
        } else {
            log_debug("Semaphore closed.");
        }
        // 注意：通常由创建者 unlink，这里不主动 unlink
        // sem_unlink(SEM_NAME);
        semaphore = SEM_FAILED;
    }

    // 注意：通常由创建者 unlink，这里不主动 unlink 共享内存对象
    // shm_unlink(SHM_NAME);

    log_info("Cleanup completed. Exiting.");
    exit(EXIT_SUCCESS);
}

// 设置曝光和增益
int set_camera_controls()
{
    log_debug("Opening subdev /dev/v4l-subdev2...");
    int subdev_fd = open("/dev/v4l-subdev2", O_RDWR);
    if (subdev_fd < 0) {
        log_error("Failed to open subdev /dev/v4l-subdev2: %s", strerror(errno));
        return -1;
    }
    log_debug("Successfully opened subdev.");

    struct v4l2_control ctrl_exp;
    memset(&ctrl_exp, 0, sizeof(ctrl_exp));
    ctrl_exp.id = V4L2_CID_EXPOSURE;
    ctrl_exp.value = 1300;
    log_debug("Setting exposure to %d...", ctrl_exp.value);

    if (ioctl(subdev_fd, VIDIOC_S_CTRL, &ctrl_exp) < 0) {
        log_error("Failed to set exposure to %d: %s", ctrl_exp.value, strerror(errno));
    } else {
        log_info("Successfully set exposure to %d.", ctrl_exp.value);
    }

    struct v4l2_control ctrl_gain;
    memset(&ctrl_gain, 0, sizeof(ctrl_gain));
    ctrl_gain.id = V4L2_CID_ANALOGUE_GAIN;
    ctrl_gain.value = 200;
    log_debug("Setting analogue gain to %d...", ctrl_gain.value);

    if (ioctl(subdev_fd, VIDIOC_S_CTRL, &ctrl_gain) < 0) {
        log_error("Failed to set analogue gain to %d: %s", ctrl_gain.value, strerror(errno));
    } else {
        log_info("Successfully set analogue gain to %d.", ctrl_gain.value);
    }

    close(subdev_fd);
    log_debug("Closed subdev.");
    return 0;
}

// 采集一帧 NV12
int capture_nv12_frame(const char *output_file)
{
    log_info("Starting frame capture to %s...", output_file);
    log_debug("Opening video device %s...", DEVICE);
    int fd = open(DEVICE, O_RDWR);
    if (fd < 0) {
        log_error("Failed to open video device %s: %s", DEVICE, strerror(errno));
        return -1;
    }
    log_debug("Successfully opened video device.");

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = WIDTH;
    fmt.fmt.pix_mp.height = HEIGHT;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    log_debug("Setting format: %dx%d, NV12...", WIDTH, HEIGHT);

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        log_error("Failed to set video format: %s", strerror(errno));
        close(fd);
        return -1;
    }
    log_debug("Successfully set video format.");

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 3;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    log_debug("Requesting %d buffers...", req.count);

    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        log_error("Failed to request buffers: %s", strerror(errno));
        close(fd);
        return -1;
    }
    log_debug("Successfully requested buffers.");

    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    buf.m.planes = planes;
    buf.length = 1; // 假设只使用一个平面
    log_debug("Querying buffer info for index %d...", buf.index);

    if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
        log_error("Failed to query buffer: %s", strerror(errno));
        close(fd);
        return -1;
    }
    log_debug("Successfully queried buffer. Length: %u, Offset: %u",
              buf.m.planes[0].length, buf.m.planes[0].m.mem_offset);

    log_debug("Mapping buffer memory...");
    void *buffer_start = mmap(NULL, buf.m.planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.planes[0].m.mem_offset);
    if (buffer_start == MAP_FAILED) {
        log_error("Failed to mmap buffer: %s", strerror(errno));
        close(fd);
        return -1;
    }
    log_debug("Successfully mapped buffer memory.");

    log_debug("Queueing buffer (index %d)...", buf.index);
    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        log_error("Failed to queue buffer: %s", strerror(errno));
        munmap(buffer_start, buf.m.planes[0].length);
        close(fd);
        return -1;
    }
    log_debug("Successfully queued buffer.");

    log_debug("Starting streaming (type %d)...", buf.type);
    if (ioctl(fd, VIDIOC_STREAMON, &buf.type) < 0) {
        log_error("Failed to start streaming: %s", strerror(errno));
        munmap(buffer_start, buf.m.planes[0].length);
        close(fd);
        return -1;
    }
    log_debug("Streaming started.");

    log_debug("Dequeuing buffer (waiting for frame)...");
    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        log_error("Failed to dequeue buffer (capture frame): %s", strerror(errno));
        ioctl(fd, VIDIOC_STREAMOFF, &buf.type);
        munmap(buffer_start, buf.m.planes[0].length);
        close(fd);
        return -1;
    }
    log_info("Frame captured successfully. Size: %u bytes.", buf.m.planes[0].bytesused);

    log_debug("Opening output file %s for writing...", output_file);
    FILE *fp = fopen(output_file, "wb");
    if (!fp) {
        log_error("Failed to open output file %s: %s", output_file, strerror(errno));
        ioctl(fd, VIDIOC_STREAMOFF, &buf.type);
        munmap(buffer_start, buf.m.planes[0].length);
        close(fd);
        return -1;
    }

    log_debug("Writing %u bytes to output file...", buf.m.planes[0].bytesused);
    size_t written = fwrite(buffer_start, buf.m.planes[0].bytesused, 1, fp);
    if (written != 1) {
        log_error("Failed to write complete frame to file: %s", strerror(errno));
        fclose(fp);
        ioctl(fd, VIDIOC_STREAMOFF, &buf.type);
        munmap(buffer_start, buf.m.planes[0].length);
        close(fd);
        return -1;
    }
    fclose(fp);
    log_info("Frame data written to %s.", output_file);

    log_debug("Stopping streaming...");
    if (ioctl(fd, VIDIOC_STREAMOFF, &buf.type) < 0) {
         log_error("Failed to stop streaming (ignoring): %s", strerror(errno));
    } else {
        log_debug("Streaming stopped.");
    }

    log_debug("Unmapping buffer memory...");
    if (munmap(buffer_start, buf.m.planes[0].length) == -1) {
        log_error("Failed to unmap buffer: %s", strerror(errno));
    } else {
        log_debug("Buffer memory unmapped.");
    }

    close(fd);
    log_debug("Video device closed.");
    log_info("Frame capture completed successfully.");
    return 0;
}

// 处理拍照、压缩和传输
void process_capture()
{
    log_info("=== Starting Capture Process ===");
    // 设置相机控制参数
    log_debug("Setting camera controls...");
    if (set_camera_controls() != 0) {
        log_error("Failed to set camera controls. Continuing with capture...");
        // 根据需求决定是否继续或返回
        // return; // 如果认为控制失败是致命的，可以取消注释
    } else {
        log_debug("Camera controls set successfully.");
    }

    // 拍照并保存为 NV12 格式
    log_debug("Capturing NV12 frame...");
    if (capture_nv12_frame("/opt/frame_nv12.raw") != 0) {
        log_error("Capture failed.");
        return;
    }
    log_info("NV12 frame captured successfully.");

    // 使用 ffmpeg 压缩为 JPEG 格式
    log_info("Running ffmpeg to convert NV12 to JPEG...");
    log_debug("Executing command: ffmpeg -y -f rawvideo -pixel_format nv12 -s 1920x1080 -i /opt/frame_nv12.raw -vf scale=512:288 -q:v 5 -f image2 /opt/frame.jpg");
    int ffmpeg_result = system("ffmpeg -y -f rawvideo -pixel_format nv12 -s 1920x1080 -i /opt/frame_nv12.raw -vf scale=512:288 -q:v 5 -f image2 /opt/frame.jpg");
    if (ffmpeg_result == -1) {
        log_error("system() call for ffmpeg failed: %s", strerror(errno));
        return;
    } else if (WIFEXITED(ffmpeg_result) && WEXITSTATUS(ffmpeg_result) != 0) {
        log_error("FFmpeg conversion failed with exit code: %d", WEXITSTATUS(ffmpeg_result));
        return;
    } else if (WIFSIGNALED(ffmpeg_result)) {
        log_error("FFmpeg conversion was terminated by signal: %d", WTERMSIG(ffmpeg_result));
        return;
    }
    log_info("FFmpeg conversion completed successfully. Output: /opt/frame.jpg");

    // 触发 BLE 传输
    log_info("Triggering BLE transmission...");
    log_debug("Copying JPEG to /tmp/123.jpg...");
    int cp_result = system("cp /opt/frame.jpg /tmp/123.jpg");
    if (cp_result == -1) {
        log_error("system() call for cp failed: %s", strerror(errno));
        return;
    } else if (WIFEXITED(cp_result) && WEXITSTATUS(cp_result) != 0) {
        log_error("Failed to copy JPEG to /tmp: %d", WEXITSTATUS(cp_result));
        return;
    } else if (WIFSIGNALED(cp_result)) {
        log_error("cp command was terminated by signal: %d", WTERMSIG(cp_result));
        return;
    }

    log_debug("Creating trigger file /tmp/send...");
    int touch_result = system("touch /tmp/send");
    
    if (touch_result == -1) {
        log_error("system() call for touch failed: %s", strerror(errno));
        return;
    } else if (WIFEXITED(touch_result) && WEXITSTATUS(touch_result) != 0) {
        log_error("Failed to create trigger file /tmp/send: %d", WEXITSTATUS(touch_result));
        return;
    } else if (WIFSIGNALED(touch_result)) {
        log_error("touch command was terminated by signal: %d", WTERMSIG(touch_result));
        return;
    }

    // 发送"PhotoCaptured"信号到共享内存
    log_debug("Sending PhotoCaptured signal to shared memory...");
    const char* photo_captured_msg = "PhotoCaptured";
    memset(shared_memory, 0, BUFFER_SIZE); // 清空共享内存
    strncpy((char*)shared_memory, photo_captured_msg, BUFFER_SIZE - 1);
    ((char*)shared_memory)[BUFFER_SIZE - 1] = '\0'; // 确保字符串结束
    
    // 通知其他进程
    if (semaphore != SEM_FAILED) {
        if (sem_post(semaphore) == -1) {
            log_error("Failed to post semaphore for PhotoCaptured signal: %s", strerror(errno));
        } else {
            log_info("PhotoCaptured signal sent successfully to shared memory.");
        }
    } else {
        log_error("Semaphore not available, cannot notify other processes.");
    }

    log_info("BLE transmission triggered successfully.");
    log_info("=== Capture Process Completed ===");
}

// 主函数
int main()
{
    log_info("=== Launch Program Started ===");

    // 注册信号处理函数 (与 display/main.c 保持一致)
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    // --- 初始化 IPC (监听已存在的 display_sem 和 display_shm) ---
    // 1. 打开已存在的共享内存 (由 display 程序创建)
    log_debug("Opening existing shared memory object %s...", SHM_NAME);
    shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        log_error("shm_open failed: %s", strerror(errno));
        cleanup(0); // 使用 cleanup 进行统一清理
        return -1;
    }
    log_debug("Shared memory object opened successfully (fd: %d).", shm_fd);

    // 2. 映射共享内存到进程地址空间
    log_debug("Mapping shared memory...");
    shared_memory = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_memory == MAP_FAILED) {
        log_error("mmap failed: %s", strerror(errno));
        cleanup(0);
        return -1;
    }
    log_debug("Shared memory mapped successfully at %p.", shared_memory);

    // 3. 关闭文件描述符 (因为我们已经映射了)
    close(shm_fd);
    shm_fd = -1; // 标记为已关闭
    log_debug("Shared memory file descriptor closed.");

    // 4. 打开已存在的信号量 (由 display 程序创建)
    log_debug("Opening existing semaphore %s...", SEM_NAME);
    semaphore = sem_open(SEM_NAME, 0); // 不创建，只打开
    if (semaphore == SEM_FAILED) {
        log_error("sem_open failed: %s", strerror(errno));
        cleanup(0);
        return -1;
    }
    log_debug("Semaphore opened successfully.");
    // --- IPC 初始化完成 ---

    // 初始化显示客户端（连接到 ai_display_service）
    if (init_display_client() != 0) {
        log_error("Warning: display client init failed, BLE text display disabled");
        // 不退出 —— 拍照功能仍可正常工作
    }

    log_info("Listening for signals on shared memory %s via semaphore %s (created by display program)...", SHM_NAME, SEM_NAME);

    // --- 主循环：监听共享内存变化 (逻辑与 display/main.c 中的 display_update_thread 类似) ---
    char last_message[BUFFER_SIZE] = {0};
    while (running) { // 使用 running 标志控制循环
        // 等待信号量
        log_debug("Waiting on semaphore...");
        if (sem_wait(semaphore) == -1) {
            if (errno == EINTR) {
                log_info("sem_wait interrupted by signal, checking running flag...");
                continue; // 处理中断信号，检查 running 标志
            }
            log_error("sem_wait failed: %s", strerror(errno));
            break; // Exit loop on other errors
        }
        log_debug("Semaphore acquired.");

        // 检查共享内存中的信号
        char current_message[BUFFER_SIZE] = {0}; // 初始化为0
        // 使用 memcpy 确保不会因为源内存未完全初始化而读取垃圾
        memcpy(current_message, (char *)shared_memory, BUFFER_SIZE - 1);
        current_message[BUFFER_SIZE - 1] = '\0'; // 确保字符串结束
        log_info("Received signal from shared memory: '%s'", current_message); // 打印原始信号

        // --- 核心逻辑：解析信号并触发拍照 ---
        // 移除重复检查，每次都处理
        if (strlen(current_message) > 0) {
            strncpy(last_message, current_message, BUFFER_SIZE - 1);
            log_info("Raw current_message (hex): ");
            for (int i = 0; i < 20 && current_message[i]; i++) {
                printf("%02X ", (unsigned char)current_message[i]);
            }
            printf("\n");
            // 解析信号 (根据需求触发拍照)
            // 与 display/main.c 不同，这里只关心 "BLE:" 开头的信号
            if (strncmp(current_message, "BLE:", 4) == 0) {
                // 所有 BLE 消息（拍照指令 / 显示文字）统一交给 handle_ble_message
                handle_ble_message(current_message);
            } else if (strlen(current_message) > 0) {
                log_info("Non-BLE signal received: '%s'. Ignoring.", current_message);
            } else {
                log_debug("Received empty signal (likely spurious wake-up).");
            }
        }
        // --- 信号处理完成 ---

        // 注意：不像 display/main.c 那样清空 shared_memory，
        // 因为其他进程（如 btgatt-server）可能需要读取它。
        // 如果需要清空，取消下面一行的注释。
        // memset(shared_memory, 0, BUFFER_SIZE);
    }
    // --- 主循环结束 ---

    // 正常退出时也会调用 cleanup
    log_info("Main loop exited. Calling cleanup...");
    cleanup(0);
    return 0;
}
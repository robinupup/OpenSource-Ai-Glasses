/**
 * my-app app 子模块 - 共用的 UI 句柄与辅助函数
 *
 * 设计：三个 lumina 子应用（场景单词=多模态百科 / 英语对练=实时翻译 / 拍照搜题）
 * 共享同一套"运行中页面"控件：
 *   - asr_label    顶部实时 ASR 原文
 *   - content_label 中间流式内容
 *   - status_label 底部状态条（IDLE/REC/WAIT/RESULT/ERR/...）
 *   - crop_img     vlm 专用：image_crop 缩图
 *   - tree_labels  homework 专用：知识树第一层 3 个节点
 *
 * main.c 在 UI 初始化时把这些 LVGL 对象指针装配到 g_app_ui，再给 app_xxx 使用。
 * ws 回调运行在 mongoose poll 线程，需要用 app_ui_lock/unlock 包裹 LVGL 调用。
 */
#ifndef MYAPP_APP_COMMON_H
#define MYAPP_APP_COMMON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

typedef struct {
    /* 通用 */
    lv_obj_t *asr_label;
    lv_obj_t *content_label;
    lv_obj_t *status_label;
    /* vlm */
    lv_obj_t *crop_img;
    /* homework */
    lv_obj_t *tree_labels[3];
} app_ui_t;

/** 装配 UI 句柄（在主线程里调一次即可）。 */
void app_common_set_ui(const app_ui_t *ui);
const app_ui_t *app_common_get_ui(void);

/** LVGL 访问互斥，由 main.c 通过 app_common_bind_lock 注入。 */
typedef void (*lock_fn_t)(void);
void app_common_bind_lock(lock_fn_t lock, lock_fn_t unlock);
void app_ui_lock(void);
void app_ui_unlock(void);

/** 便捷：给 LVGL label 安全地设置文本（自动加锁、NULL 保护） */
void app_ui_set_text(lv_obj_t *label, const char *text);
void app_ui_append_text(lv_obj_t *label, const char *piece);
void app_ui_clear_text(lv_obj_t *label);

/** 便捷：ASR 显示控制
 *  - show：在 asr_label 上显示文字（会自动把 asr_label 从隐藏状态恢复显示）
 *  - hide：隐藏 asr_label 并清空文字；用于结果返回后"把 ASR 去掉"
 *  规则：所有功能一律"先显示 ASR 内容、结果返回即隐藏"。*/
void app_ui_show_asr(const char *text);
void app_ui_hide_asr(void);

/** 便捷：把 vlm 的 image_crop base64 解码并渲染到 crop_img。 */
void app_ui_show_crop_b64_jpeg(const char *b64);

/** 便捷：homework 第一层节点。最多 3 个；为 NULL 的格子置空。 */
void app_ui_set_tree_layer1(const char *n0, const char *n1, const char *n2);

/**
 * 收音状态指示器（左上角常驻 label，由 main.c 创建后注册）。
 * - app_common_set_mic_indicator(label) 只在启动时由主线程调一次
 * - app_common_set_mic_on(on)  各 app 在 mic 启/停前后调用，on=1 显示"收音：开"
 */
/** 清空 crop 图片（隐藏上一轮缩略图）。 */
void app_ui_clear_crop(void);

void app_common_set_mic_indicator(lv_obj_t *label);
void app_common_set_mic_on(int on);

/** 生成 uuid（简易版，基于 /proc/sys/kernel/random/uuid，退回到时间+随机）。 */
void app_gen_uuid(char *out, size_t out_len);

/** main.c 提供：采一帧相片到 jpeg 文件，返回 0 成功。线程安全。 */
int  myapp_take_photo(char *out_path, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif

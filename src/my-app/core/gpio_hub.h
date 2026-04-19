/**
 * my-app core - GPIO 事件中心
 *
 * 复刻 ai-core 的 "GPIO Event Hub" 设计思想：统一管理多个 GPIO，
 * 以事件形式通过 event_bus 广播给所有订阅者。
 *
 * 事件 topic 约定：
 *   "gpio/press"    - 按键按下
 *   "gpio/release"  - 按键释放
 *   "gpio/<n>/press"、"gpio/<n>/release"  - 特定 GPIO（最多发布这两个）
 *
 * payload 结构：gpio_event_payload_t
 *
 * 实现方式：直接通过 /sys/class/gpio 轮询，无需 ai-core 服务。
 */

#ifndef MYAPP_GPIO_HUB_H
#define MYAPP_GPIO_HUB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GPIO_EVT_PRESS   = 1,
    GPIO_EVT_RELEASE = 2,
    GPIO_EVT_ERROR   = 3,
} gpio_evt_type_t;

typedef struct {
    int      gpio;              /* GPIO 编号 */
    int      type;              /* gpio_evt_type_t */
    int      level;             /* 电平 0/1 */
    int      press_duration_ms; /* 仅 release 事件有效 */
    uint64_t timestamp_us;
} gpio_event_payload_t;

/** GPIO 激活方式（低/高电平有效） */
typedef enum {
    GPIO_ACTIVE_LOW  = 0,
    GPIO_ACTIVE_HIGH = 1,
} gpio_active_t;

/** 初始化 GPIO 中心（启动轮询线程）。必须先 event_bus_init()。*/
int  gpio_hub_init(void);

/** 添加一个 GPIO 监控。重复添加幂等。 */
int  gpio_hub_add(int gpio, gpio_active_t active);

/** 移除监控。 */
int  gpio_hub_remove(int gpio);

/** 直接查询当前状态（1=按下 0=释放 -1=错误） */
int  gpio_hub_get_state(int gpio);

/** 关闭并清理。 */
void gpio_hub_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

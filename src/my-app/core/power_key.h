/**
 * my-app core - 物理电源键监听
 *
 * 内核 DTS 把 GPIO1 作为 "GPIO Key Power" 交给 gpio_keys 驱动，
 * 因此不能用 sysfs 直接读，必须从 input 子系统 /dev/input/eventN 里
 * 取 KEY_POWER 事件。本模块后台线程监听，并把按下/抬起事件
 * 以 "gpio/press"、"gpio/release" 主题发布到 event_bus（gpio 编号固定为 1），
 * 让 main.c 里原有的 gpio 处理逻辑直接复用。
 */
#ifndef MYAPP_POWER_KEY_H
#define MYAPP_POWER_KEY_H

#ifdef __cplusplus
extern "C" {
#endif

/** 固定映射到 event_bus 里 gpio/press 事件的 gpio 编号 */
#define POWER_KEY_GPIO 1

int  power_key_init(void);
void power_key_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif

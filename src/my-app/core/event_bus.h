/**
 * =============================================================================
 * my-app core - 轻量级事件总线（进程内 Pub/Sub）
 * =============================================================================
 *
 * 设计思想复刻自 ai-core 的 GPIO Event Hub：
 *   - 单一事件中心，多订阅者
 *   - 每个事件有 topic（主题）+ payload（数据）+ 时间戳 + 序列号
 *   - 线程安全；回调在独立分发线程中触发
 *   - 环形队列缓存历史事件，订阅者按自己的 sequence 拉取
 *
 * 与 ai-core 的差异：
 *   - 此实现为进程内（同进程多线程），无 IPC 共享内存/Unix Socket
 *   - 事件 topic 用字符串而非固定 GPIO 编号，可扩展至任意模块
 *
 * 使用示例：
 *   event_bus_init();
 *   event_bus_subscribe("gpio/press", on_gpio_press, user_data);
 *   event_bus_publish("gpio/press", &payload, sizeof(payload));
 */

#ifndef MYAPP_EVENT_BUS_H
#define MYAPP_EVENT_BUS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVENT_BUS_TOPIC_MAX       64
#define EVENT_BUS_PAYLOAD_MAX    512
#define EVENT_BUS_QUEUE_SIZE     128
#define EVENT_BUS_MAX_SUBSCRIBERS 64

typedef struct {
    char     topic[EVENT_BUS_TOPIC_MAX];
    uint32_t sequence;
    uint64_t timestamp_us;
    size_t   payload_size;
    uint8_t  payload[EVENT_BUS_PAYLOAD_MAX];
} event_t;

/**
 * 事件回调。回调在事件总线的分发线程中被调用，需注意线程安全。
 * topic 支持通配符：以 "/" 分段匹配，"*" 表示单段任意。
 *   例如订阅 "gpio/" + "*"  可匹配 "gpio/press"、"gpio/release"。
 */
typedef void (*event_callback_t)(const event_t *evt, void *user_data);

/** 初始化事件总线，启动分发线程。返回 0 成功，-1 失败。可重复调用幂等。 */
int  event_bus_init(void);

/** 关闭事件总线，停止分发线程，清理资源。 */
void event_bus_shutdown(void);

/**
 * 订阅事件。
 * @param topic_pattern 主题模式（支持 "*" 通配）
 * @param cb 回调
 * @param user_data 透传数据
 * @return >=0 订阅者 id；<0 失败
 */
int  event_bus_subscribe(const char *topic_pattern, event_callback_t cb, void *user_data);

/** 按订阅 id 取消订阅。 */
void event_bus_unsubscribe(int subscriber_id);

/**
 * 发布事件。
 * @param topic 主题
 * @param payload 可为 NULL
 * @param payload_size 字节数，<= EVENT_BUS_PAYLOAD_MAX
 * @return 0 成功，-1 失败
 */
int  event_bus_publish(const char *topic, const void *payload, size_t payload_size);

/** 获取当前微秒级时间戳 */
uint64_t event_bus_now_us(void);

#ifdef __cplusplus
}
#endif

#endif /* MYAPP_EVENT_BUS_H */

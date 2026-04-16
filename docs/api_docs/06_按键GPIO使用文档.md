# 按键 GPIO 使用文档

> **适用平台**: OSAIG 开源 AI 眼镜 (RV1106B)
> **GPIO 子系统**: Linux sysfs GPIO / ai-core GPIO Hub
> **按键数量**: 2 个物理按键 + 1 个保留 GPIO

---

## 1. 概述

眼镜上配备了物理按键，通过 GPIO 输入实现用户交互。项目提供了两种 GPIO 使用方式：

1. **SDK GPIO Hub API**（推荐）：通过 `ai_gpio.h` 对接 `ai-core` 的 GPIO 事件中心
2. **sysfs 直接轮询**：通过 `/sys/class/gpio` 直接读取 GPIO 电平

**关键文件：**

| 文件 | 作用 |
|------|------|
| `SDK/ai_glass_sdk/include/ai_gpio.h` | GPIO 事件 API 头文件 |
| `SDK/ai_glass_sdk/docs/GPIO_Client_API.md` | API 文档 |
| `SDK/ai_glass_sdk/examples/gpio_example/` | GPIO 示例 |
| `src/launcher-app/main.c` | Launcher：GPIO Hub 订阅实现 |
| `src/display-service/main.c` | 显示服务：GPIO 唤醒实现 |
| `src/touchpad_manager/launch.cpp` | sysfs 轮询实现 |

---

## 2. GPIO 引脚定义

### 2.1 当前使用的 GPIO

| GPIO 编号 | 位置 | 功能 | 说明 |
|-----------|------|------|------|
| **GPIO 0** | 右镜腿 | 翻页键 / 菜单切换 | Launcher 中用于切换菜单项 |
| **GPIO 1** | — | 保留 | 显示服务监控唤醒用 |
| **GPIO 75** | 左镜腿 | 确认键 / 功能触发 | Launcher 中用于确认选择 |

### 2.2 各组件的 GPIO 使用

| 组件 | GPIO 0 | GPIO 1 | GPIO 75 |
|------|--------|--------|---------|
| **launcher-app** | 翻页 (`GPIO_PAGE`) | — | 确认 (`GPIO_CONFIRM`) |
| **display-service** | 唤醒屏幕 | 唤醒屏幕 | 唤醒屏幕 |
| **touchpad_manager** | 菜单切换 (IOB) | — | 功能执行 (IOA) |

---

## 3. 方式一：SDK GPIO Hub API（v2.0 推荐）

### 3.1 架构

```
┌──────────────┐                    ┌──────────────┐
│  应用程序 A   │───┐                │   ai-core    │
└──────────────┘   │  共享内存+Socket │  GPIO Hub    │
┌──────────────┐   │◄───────────────►│              │◄──── GPIO 硬件
│  应用程序 B   │───┘                │  服务端       │
└──────────────┘                    └──────────────┘
```

- **IPC**：共享内存 (`/ai_gpio_event_hub`, 8KB) + Unix Socket (`/tmp/ai_gpio_event_hub_broadcast`)
- **单一连接**：一个客户端即可订阅多个 GPIO（v2.0 优势）
- **多客户端**：支持最多 64 个并发客户端

### 3.2 事件类型

```c
typedef enum {
    GPIO_EVENT_PRESS    = 1,    // 按键按下
    GPIO_EVENT_RELEASE  = 2,    // 按键释放
    GPIO_EVENT_ERROR    = 3     // GPIO 错误
} gpio_event_t;
```

### 3.3 Hub 客户端 API

| 函数 | 描述 |
|------|------|
| `ai_gpio_hub_client_create(&client)` | 创建客户端实例 |
| `ai_gpio_hub_client_connect(&client)` | 连接到事件中心 |
| `ai_gpio_hub_client_subscribe_gpios(&client, list, count, cb, data)` | 订阅指定 GPIO 列表 |
| `ai_gpio_hub_client_subscribe_all(&client, cb, data)` | 订阅所有 GPIO |
| `ai_gpio_hub_client_get_gpio_state(&client, gpio)` | 获取 GPIO 当前状态 |
| `ai_gpio_hub_client_get_active_gpios(&client, list, max)` | 获取活跃 GPIO 列表 |
| `ai_gpio_hub_client_unsubscribe(&client)` | 取消订阅 |
| `ai_gpio_hub_client_disconnect(&client)` | 断开连接 |
| `ai_gpio_hub_client_destroy(&client)` | 销毁客户端 |
| `ai_gpio_hub_client_is_service_alive(&client)` | 检查服务可用性 |

### 3.4 最小示例

```c
#include "ai_gpio.h"
#include <stdio.h>
#include <unistd.h>

void hub_callback(gpio_event_t event, int gpio, void *data) {
    const char *evt_str;
    switch (event) {
        case GPIO_EVENT_PRESS:   evt_str = "按下"; break;
        case GPIO_EVENT_RELEASE: evt_str = "释放"; break;
        case GPIO_EVENT_ERROR:   evt_str = "错误"; break;
        default: evt_str = "未知"; break;
    }
    printf("GPIO%d %s\n", gpio, evt_str);
}

int main() {
    gpio_event_hub_client_t client;

    // 1. 创建并连接
    ai_gpio_hub_client_create(&client);
    if (ai_gpio_hub_client_connect(&client) != 0) {
        printf("连接失败，请确保 ai-core 已启用 GPIO\n");
        return -1;
    }

    // 2. 订阅 GPIO 0 和 75
    int gpios[] = {0, 75};
    ai_gpio_hub_client_subscribe_gpios(&client, gpios, 2,
                                        hub_callback, NULL);

    // 3. 等待事件
    printf("监听中，按 Ctrl+C 退出...\n");
    while (1) sleep(1);

    // 4. 清理
    ai_gpio_hub_client_destroy(&client);
    return 0;
}
```

### 3.5 Launcher 中的实际使用

Launcher 应用展示了 GPIO Hub 在 UI 交互中的完整用法：

```c
// src/launcher-app/main.c

#define GPIO_PAGE     0    // 翻页键
#define GPIO_CONFIRM  75   // 确认键

// GPIO 回调（在独立线程中执行）
static void gpio_hub_callback(gpio_event_t event, int gpio, void *data) {
    if (event == GPIO_EVENT_PRESS) {
        pthread_mutex_lock(&ui_mutex);
        pending_gpio_event = gpio;
        ui_update_pending = 1;
        pthread_mutex_unlock(&ui_mutex);
    }
}

// 初始化 GPIO 客户端
int init_gpio_clients(void) {
    ai_gpio_hub_client_create(&gpio_hub_client);
    if (ai_gpio_hub_client_connect(&gpio_hub_client) == 0) {
        int gpios[] = {GPIO_PAGE, GPIO_CONFIRM};
        ai_gpio_hub_client_subscribe_gpios(&gpio_hub_client, gpios, 2,
                                            gpio_hub_callback, NULL);
    }
    return 0;
}

// 主循环中处理 GPIO 事件（线程安全）
void main_loop() {
    while (running) {
        if (ui_update_pending) {
            pthread_mutex_lock(&ui_mutex);
            int gpio = pending_gpio_event;
            pending_gpio_event = -1;
            ui_update_pending = 0;
            pthread_mutex_unlock(&ui_mutex);

            if (gpio == GPIO_PAGE) {
                handle_page_key();     // 翻页
            } else if (gpio == GPIO_CONFIRM) {
                handle_confirm_key();  // 确认
            }
        }
        lv_timer_handler();
        usleep(5000);
    }
}
```

> **线程安全**：GPIO 回调在独立线程中执行，不应直接操作 LVGL。通过 `pending_gpio_event` 传递到主循环处理。

### 3.6 显示服务中的唤醒用法

```c
// src/display-service/main.c

#define GPIO_KEY_0   0
#define GPIO_KEY_1   1
#define GPIO_KEY_75  75

void gpio_wakeup_callback(gpio_event_t event, int gpio, void *data) {
    last_activity_time = time(NULL);

    if (display_off) {
        send_cmd(SPI_DISPLAY_ENABLE);
        send_cmd(SPI_SYNC);
        display_off = 0;
        printf("Display woken by GPIO %d\n", gpio);
    }
}

// 订阅 3 个 GPIO 用于唤醒
int gpios[] = {GPIO_KEY_0, GPIO_KEY_1, GPIO_KEY_75};
ai_gpio_hub_client_subscribe_gpios(&gpio_hub_client, gpios, 3,
                                    gpio_wakeup_callback, NULL);
```

---

## 4. 方式一兼容：v1.1 单 GPIO 客户端 API

v2.0 SDK 完全向后兼容 v1.1 API：

```c
#include "ai_gpio.h"

void my_callback(gpio_event_t event, int gpio, void *data) {
    if (event == GPIO_EVENT_PRESS) {
        printf("按键按下\n");
    }
}

int main() {
    gpio_event_client_t client = {0};

    // 创建并连接
    ai_gpio_event_client_create(&client);
    ai_gpio_event_client_connect(&client);  // 默认连接 GPIO 1

    // 连接到指定 GPIO (v1.1)
    // ai_gpio_event_client_connect_gpio(&client, 0);  // 连接 GPIO 0

    // 订阅事件
    ai_gpio_event_client_subscribe(&client, my_callback, NULL);

    while (1) sleep(1);

    // 清理
    ai_gpio_event_client_unsubscribe(&client);
    ai_gpio_event_client_destroy(&client);
    return 0;
}
```

### v1.1 → v2.0 迁移指南

| v1.1 (旧) | v2.0 (新) |
|------------|-----------|
| `gpio_event_client_t` | `gpio_event_hub_client_t` |
| `ai_gpio_event_client_create()` | `ai_gpio_hub_client_create()` |
| `ai_gpio_event_client_connect()` | `ai_gpio_hub_client_connect()` |
| `ai_gpio_event_client_subscribe()` | `ai_gpio_hub_client_subscribe_gpios()` |
| `ai_gpio_event_client_destroy()` | `ai_gpio_hub_client_destroy()` |

---

## 5. 方式二：sysfs 直接轮询

### 5.1 GPIO 导出与配置

```c
// src/touchpad_manager/launch.cpp
#define GPIO_SYSFS_PATH  "/sys/class/gpio"
#define POLL_INTERVAL_MS 50    // 轮询间隔 50ms

// 导出 GPIO
void export_gpio(int gpio_num) {
    char path[64];
    snprintf(path, sizeof(path), "%s/export", GPIO_SYSFS_PATH);
    FILE *fp = fopen(path, "w");
    fprintf(fp, "%d", gpio_num);
    fclose(fp);
}

// 设置方向
void set_gpio_direction(int gpio_num, const char *direction) {
    char path[64];
    snprintf(path, sizeof(path), "%s/gpio%d/direction",
             GPIO_SYSFS_PATH, gpio_num);
    FILE *fp = fopen(path, "w");
    fprintf(fp, "%s", direction);  // "in" 或 "out"
    fclose(fp);
}

// 读取电平
int read_gpio_state(int gpio_num) {
    char path[64], value_str[4];
    snprintf(path, sizeof(path), "%s/gpio%d/value",
             GPIO_SYSFS_PATH, gpio_num);
    FILE *fp = fopen(path, "r");
    fgets(value_str, sizeof(value_str), fp);
    fclose(fp);
    return atoi(value_str);
}
```

### 5.2 轮询监控

```c
typedef struct {
    int gpio_num;
    int prev_state;
    int current_state;
    int debounce_count;
} GPIO_STATE;

void *gpio_monitor_thread(void *arg) {
    GPIO_STATE gpios[2] = {
        {75, -1, -1, 0},   // GPIO 75 - 确认键
        {0,  -1, -1, 0}    // GPIO 0  - 翻页键
    };

    // 导出并配置 GPIO
    for (int i = 0; i < 2; i++) {
        export_gpio(gpios[i].gpio_num);
        set_gpio_direction(gpios[i].gpio_num, "in");
        gpios[i].prev_state = read_gpio_state(gpios[i].gpio_num);
    }

    // 轮询循环
    while (running) {
        for (int i = 0; i < 2; i++) {
            gpios[i].current_state = read_gpio_state(gpios[i].gpio_num);

            // 检测下降沿（高→低 = 按下）
            if (gpios[i].prev_state == 1 && gpios[i].current_state == 0) {
                handle_button_press(gpios[i].gpio_num);
            }

            gpios[i].prev_state = gpios[i].current_state;
        }

        usleep(POLL_INTERVAL_MS * 1000);
    }
    return NULL;
}
```

### 5.3 按键动作处理

在 touchpad_manager 中，按键触发不同的功能：

| GPIO | MenuValue | 动作 |
|------|-----------|------|
| GPIO 0 (IOB) | — | 递增 MenuValue，发消息到显示端 |
| GPIO 75 (IOA) | 1 | 启动/停止 AI 客户端 |
| GPIO 75 (IOA) | 2 | 切换 AI 模式 |
| GPIO 75 (IOA) | 3 | 拍照 |
| GPIO 75 (IOA) | 4 | 开始/停止录像 |

---

## 6. 性能指标

| 指标 | SDK Hub | sysfs 轮询 |
|------|---------|------------|
| 事件延迟 | < 13ms | ~50ms (取决于轮询间隔) |
| CPU 占用 | < 1% | 略高（持续轮询） |
| 内存占用 | 4~8KB (SHM) | 极低 |
| 并发客户端 | 64 个 | 不适用 |
| 多 GPIO 支持 | 单连接多 GPIO | 需自行管理 |

---

## 7. 共享内存布局（Hub）

```c
// GPIO Hub 共享内存结构
typedef struct {
    volatile uint32_t magic;            // 0x47504855 = "GPHU"
    volatile int service_running;
    uint64_t service_start_time;
    uint64_t last_heartbeat_time;

    // 每个 GPIO 的状态
    gpio_hub_gpio_state_t gpio_states[16];  // 最多 16 个 GPIO
    int active_gpio_count;

    // 统一事件队列
    gpio_event_data_t event_queue[64];
    volatile uint32_t queue_write_index;
    volatile uint32_t broadcast_sequence;

    uint64_t total_event_count;
    volatile int client_count;
} gpio_event_hub_shm_t;
```

每个 GPIO 的状态信息：

```c
typedef struct {
    int gpio_number;        // GPIO 编号（-1 = 未使用）
    int is_active;          // 是否已激活监控
    int current_state;      // 当前电平
    int is_pressed;         // 按键逻辑状态
    uint64_t last_event_time;
    uint64_t press_count;
    uint64_t release_count;
} gpio_hub_gpio_state_t;
```

---

## 8. 服务端启动

```bash
# 启用 GPIO 监控（单个 GPIO）
./ai-core --enable-gpio --gpio-number 75

# 启用多个 GPIO
./ai-core --enable-gpio --gpio-numbers 0,1,75
```

---

## 9. 编译链接

```bash
arm-rockchip831-linux-uclibcgnueabihf-gcc \
    -o gpio_app gpio_app.c \
    -I/path/to/ai_glass_sdk/include \
    -L/path/to/ai_glass_sdk/lib \
    -lai_glass_sdk -lpthread -lrt
```

---

## 10. 注意事项

1. **避免冲突**：同一 GPIO 不应被 SDK Hub 和 sysfs 轮询同时使用
2. **ai-core 先启动**：使用 Hub API 前确保 `ai-core --enable-gpio` 已运行
3. **回调线程安全**：GPIO 回调在独立线程中执行，操作 UI 需通过消息传递
4. **去抖动**：sysfs 轮询方式需自行实现按键去抖动逻辑
5. **电平逻辑**：当前实现中**高→低**视为按下事件（低电平有效）
6. **权限**：sysfs GPIO 操作可能需要 root 权限
7. **v2.0 优先**：新项目建议使用 Hub API (`ai_gpio_hub_client_*`)，资源占用更低

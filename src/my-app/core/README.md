# my-app core

全开源实现，替代闭源 `ai_glass_sdk` / `ai-core` 的客户端库。所有模块
直接使用 Linux 标准接口（sysfs、V4L2、SPI、ALSA 工具、wpa_supplicant）。

## 模块一览

| 模块 | 头文件 | 说明 |
| --- | --- | --- |
| 事件总线 | `event_bus.h` | 进程内 Pub/Sub，复刻 ai-core 事件中心思想 |
| GPIO | `gpio_hub.h` | sysfs 轮询 + 事件广播，支持 `gpio/press`、`gpio/<n>/press` 等主题 |
| 喇叭 | `audio_player.h` | 通过 `aplay` 播放 WAV/PCM |
| 麦克风 | `mic_capture.h` | 通过 `arecord` 管道流式读取 PCM |
| 相机 | `camera.h` | V4L2 `/dev/video0` 单帧获取 |
| WiFi | `wifi.h` | 用 `wpa_supplicant` + `udhcpc` 完成连接，事件总线通知状态 |
| 光波导 | `display/waveguide.h` | SPI 直驱 JBD013 微显示，整屏/局部刷新 |

## 依赖关系

```
  main.c
    │
    ├──► event_bus  (所有异步通知的枢纽)
    │
    ├──► gpio_hub ──┐
    ├──► wifi     ──┼──► publish 到 event_bus
    ├──► camera   ──┘
    │
    ├──► audio_player / mic_capture  (阻塞/异步 API)
    │
    └──► display/waveguide (SPI 硬件直驱)
```

## 事件总线 topic 约定

| Topic | Payload | 说明 |
| --- | --- | --- |
| `gpio/press` | `gpio_event_payload_t` | 任意 GPIO 按下 |
| `gpio/release` | `gpio_event_payload_t` | 任意 GPIO 释放 |
| `gpio/<n>/press` | 同上 | 指定 GPIO 编号 |
| `wifi/connecting` | `wifi_status_t` | 开始连接 |
| `wifi/connected` | `wifi_status_t` | 连接成功（含 IP） |
| `wifi/disconnected` | `wifi_status_t` | 断开 |
| `wifi/failed` | `wifi_status_t` | 启动 wpa_supplicant 失败 |
| `audio/play_start` | `char file_path[]` | 开始播放 |
| `audio/play_finish` | `char file_path[]` | 播放完成（仅同步播放） |
| `camera/frame` | 元数据 | 获取到一帧 |

订阅者可使用 `*` 通配单段，例如 `gpio/*`、`wifi/*`。

## 运行时依赖

固件需提供下列工具（Rockchip 官方 rootfs 默认具备）：

- `/usr/bin/aplay`、`/usr/bin/arecord`、`/usr/bin/amixer`
- `/usr/sbin/wpa_supplicant`、`/usr/sbin/wpa_cli`
- `/sbin/udhcpc` 或 `/sbin/dhclient`

以及相应设备节点：

- `/sys/class/gpio/`
- `/dev/spidev0.0`
- `/dev/video0`
- `/dev/snd/*`

## 构建

```bash
cd src/my-app
make                # 默认：不链接 LVGL/闭源 SDK
make ENABLE_LVGL=1  # 如需重新启用 LVGL UI（基于 third_party/lvgl）
```

构建产物：`build/bin/my-app`

## 主应用流程（main.c）

1. `event_bus_init()` → `waveguide_init()` → `audio_player_init()`
2. `gpio_hub_init()` 并订阅 GPIO0 / GPIO75
3. `camera_open()` 打开相机（MJPG 1280x720）
4. `wifi_init(wlan0)` + `wifi_connect("lumina","mt123456")`
5. 监听 `wifi/connected` 事件：成功时异步 `audio_player_play_wav_async()` 播放提示音
6. 监听 `gpio/press` 事件：
   - GPIO75 按下 → 取一帧 JPEG 保存到 `/tmp/myapp_snap_*.jpg`
   - GPIO0 按下 → 切换麦克风流开关

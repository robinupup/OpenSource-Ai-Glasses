# my-app 功能与操作文档

## 概述

my-app 是基于 launcher-app 复制改造的 AI 眼镜主程序，运行在 RV1106 平台上，通过光波导显示 UI 界面，使用镜腿物理按键进行交互。

- **显示**：640×480 分辨率，4bpp 灰度，JBD013 Micro-OLED 光波导
- **图形引擎**：LVGL 8.3.11（8bit 色深）
- **交互方式**：两个 GPIO 物理按键（无触摸屏）
- **依赖服务**：`display-service`（显示）、`ai-core`/`guard`（GPIO 按键广播）

---

## 目录结构

```
my-app/
├── main.c                  # 主程序（初始化、状态机、按键处理、主循环）
├── Makefile                # 编译与部署脚本
├── ui/
│   ├── ui.h                # UI 全局变量声明
│   ├── ui.c                # UI 初始化入口（ui_init）
│   ├── ui_helpers.h/.c     # LVGL 辅助函数
│   ├── ui_events.h         # UI 事件声明
│   ├── screens/
│   │   ├── ui_Screen1.c    # 主屏幕（首页 + 子菜单）所有控件创建
│   │   └── ui_Navgation.c  # 导航屏（未使用）
│   ├── fonts/
│   │   ├── ui_font_alibaba_48.c  # 阿里巴巴普惠体 48号（主要 UI 字体）
│   │   ├── ui_font_alibaba_30.c  # 阿里巴巴普惠体 30号（已声明，未使用）
│   │   ├── ui_font_Font1.c       # 预留字体 1
│   │   └── ui_font_Font2.c       # 预留字体 2
│   ├── images/
│   │   ├── camera.c              # 相机图标
│   │   ├── ui_img_weixiao_png.c  # 微笑表情图
│   │   ├── ui_img_799793925.c    # 爱心图标
│   │   └── ui_img_lanya_png.c    # 蓝牙图标
│   └── components/
│       └── ui_comp_hook.c        # LVGL 组件钩子
└── build/                  # 编译产物（自动生成）
    ├── obj/                # 目标文件
    └── bin/
        └── my-app          # 最终二进制
```

---

## 编译与部署

### 环境要求

- ARM 交叉编译工具链：`arm-rockchip831-linux-uclibcgnueabihf-gcc`
- 工具链路径：`../../tools/linux/toolchain/arm-rockchip831-linux-uclibcgnueabihf/bin`
- AI Glass SDK：`../../SDK/ai_glass_sdk/`
- LVGL 源码：`../../third_party/lvgl/`

### 编译

```bash
cd OpenSource-Ai-Glasses/src/my-app
make clean && make
```

编译产物：`build/bin/my-app`

### 部署到眼镜

首次部署，按顺序执行以下三步（之后只有改代码时才需要重复第 1 步）：

```bash
# 1) 推送主程序
make deploy

# 2) 推送启停脚本（一次即可）
make deploy-script

# 3) 推送运行时配置 myapp.conf（WiFi / 算法服务 IP）
#    部署前先按下面"配置文件"一节把 scripts/myapp.conf.sample 改成你自己的值
make deploy-conf
```

三个目标实际执行的命令：

| Make 目标 | 实际命令 |
|-----------|---------|
| `deploy`        | `adb push build/bin/my-app /oem/usr/bin/my-app` + `chmod +x` |
| `deploy-script` | `adb push scripts/app_switch.sh /oem/usr/bin/app_switch.sh` + `chmod +x` |
| `deploy-conf`   | `adb push scripts/myapp.conf.sample /oem/etc/myapp.conf` |

---

## 配置文件（必读）

**所有可能变动的运行时参数**（WiFi 账号密码、算法服务 IP/端口）**都集中在一个配置文件里**，不要再去改源码。

- 源码里的模板：`scripts/myapp.conf.sample`
- 部署到眼镜的路径：`/oem/etc/myapp.conf`
- 加载代码：`core/config.c` (`myapp_config_load`)
- 文件不存在 / 字段缺失时，会自动回落到 `core/config.c` 内置默认值，程序**不会失败**。

### 配置项说明

```ini
# ---- WiFi（开机后自动连接） ----
wifi_ssid=CU_Ahta           # 你家/办公室路由器 SSID
wifi_password=xd39ad5z      # WiFi 密码
wifi_iface=wlan0            # 一般不用改

# ---- 算法服务（VLM / 翻译 / 拍照搜题 共用一台服务器） ----
server_host=192.168.1.7     # 算法服务所在机器的局域网 IP
vlm_port=8002               # 多模态百科
homework_port=8003          # 拍照搜题
translate_port=8004         # 实时翻译
```

### 修改配置的两种方式

**方式 A（推荐，可重现）**：改源码里的 `scripts/myapp.conf.sample`，然后：

```bash
make deploy-conf
adb shell /oem/usr/bin/app_switch.sh stop
adb shell /oem/usr/bin/app_switch.sh myapp
```

**方式 B（临时调试）**：直接在眼镜上改：

```bash
adb shell vi /oem/etc/myapp.conf
adb shell /oem/usr/bin/app_switch.sh stop
adb shell /oem/usr/bin/app_switch.sh myapp
```

> 配置加载发生在 `my-app` 启动阶段，改完必须重启进程才生效。

---

## 启动项目

目前**不启用开机自启**，所有程序都**手动启动**，用 `scripts/app_switch.sh` 这一个入口控制。脚本已随 `make deploy-script` 推送到眼镜的 `/oem/usr/bin/app_switch.sh`。

### 日常启动流程（已部署过一次之后）

```bash
# 1) 确认 adb 连得上
adb devices

# 2) 启动自研 my-app（会自动先把旧闭源栈 guard/ai-core/launcher-app 全部杀掉）
adb shell /oem/usr/bin/app_switch.sh myapp

# 3) 实时查看日志（Ctrl-C 只退出 tail，不会杀 my-app）
adb shell /oem/usr/bin/app_switch.sh log myapp
```

### app_switch.sh 子命令一览

| 命令 | 作用 |
|------|------|
| `app_switch.sh myapp`       | 杀掉其它应用，启动自研 `my-app`（纯开源路径，不依赖 guard/ai-core） |
| `app_switch.sh launcher`    | 启动旧闭源栈：`guard` → `ai-core` → `display-service` → `launcher-app` |
| `app_switch.sh stop`        | 停掉当前所有相关进程 |
| `app_switch.sh status`      | 查看哪些进程在跑 |
| `app_switch.sh log myapp`   | `tail -F /tmp/my-app.log` |
| `app_switch.sh log launcher`| `tail -F /tmp/launcher-app.log` |
| `app_switch.sh myapp -f`    | 启动后自动跟随 log（同上，一条命令搞定） |

日志文件：

- `my-app`      → `/tmp/my-app.log`
- `launcher-app`→ `/tmp/launcher-app.log`

### 首次上机的完整 Checklist

1. `cd OpenSource-Ai-Glasses/src/my-app`
2. 编辑 `scripts/myapp.conf.sample`，填入你自己的 WiFi SSID / 密码、算法服务 IP
3. `make clean && make`                  编译
4. `make deploy`                          推送 my-app
5. `make deploy-script`                   推送 app_switch.sh（只需做一次）
6. `make deploy-conf`                     推送 myapp.conf（改了配置就重新执行）
7. `adb shell /oem/usr/bin/app_switch.sh myapp -f`   启动并跟随日志

### 命令行参数

| 参数 | 说明 | 示例 |
|------|------|------|
| `-h`, `--help` | 显示帮助信息 | `my-app -h` |
| `--powersave_timeout <秒>` | 屏幕省电超时（0=禁用） | `my-app --powersave_timeout 60` |

> `app_switch.sh` 启动 my-app 时不会带任何参数，需要调参数时可以临时手动起：
> `adb shell nohup /oem/usr/bin/my-app --powersave_timeout 60 </dev/null >/tmp/my-app.log 2>&1 &`

---

## 开机自动启动（可选，默认未启用）

启动脚本位于眼镜的 `/etc/init.d/S60_app_launcher`，已配置为启动 my-app：

```bash
#!/bin/sh
export LD_LIBRARY_PATH=/oem/usr/lib:$LD_LIBRARY_PATH

# 等待 guard 和 ai-core 就绪
TIMEOUT=30
while [ $TIMEOUT -gt 0 ]; do
    if pidof guard >/dev/null && pidof ai-core >/dev/null; then
        break
    fi
    sleep 1
    TIMEOUT=$((TIMEOUT-1))
done

# 启动显示服务
/oem/usr/bin/display-service &
sleep 2

# 启动 my-app
/oem/usr/bin/my-app &
```

### 恢复原始 launcher-app

```bash
adb shell cp /userdata/init_backup/S60_app_launcher.orig /etc/init.d/S60_app_launcher
adb reboot
```

---

## 程序启动流程

```
main()
  │
  ├─ setbuf(stdout, NULL)          禁用输出缓冲
  ├─ 解析命令行参数                 --powersave_timeout / --help
  │
  ├─ init_display_client()         连接 display-service
  │   ├─ ai_display_init()         创建显示客户端
  │   ├─ ai_display_connect()      连接到显示服务（共享内存）
  │   ├─ ai_display_get_framebuffer()  获取帧缓冲区指针
  │   └─ ai_display_request_focus()    请求显示焦点
  │
  ├─ init_gpio_clients()           连接 GPIO 事件中心
  │   ├─ ai_gpio_hub_client_create()   创建 Hub 客户端
  │   ├─ ai_gpio_hub_client_connect()  连接到 ai-core 的 GPIO Hub
  │   └─ ai_gpio_hub_client_subscribe_gpios({0, 75}, callback)
  │                                    订阅 GPIO0 和 GPIO75 事件
  │
  ├─ init_lvgl()                   初始化 LVGL 图形引擎
  │   ├─ lv_init()
  │   ├─ lv_disp_draw_buf_init()   初始化绘图缓冲区（640×10行）
  │   └─ lv_disp_drv_register()    注册显示驱动（disp_flush 回调）
  │
  ├─ ui_init()                     创建所有 UI 控件
  │   ├─ ui_Screen1_screen_init()  创建主屏幕上的所有控件
  │   └─ lv_disp_load_scr(ui_Screen1)  加载主屏幕
  │
  ├─ switch_to_state(STATE_HOME)   进入首页状态
  │
  └─ while(1) 主循环               每 5ms 一次
      ├─ 检查 ui_update_pending    有按键事件待处理？
      │   ├─ GPIO_PAGE(0)  → handle_page_key()
      │   └─ GPIO_CONFIRM(75) → handle_confirm_key()
      ├─ lv_tick_inc(5)            告诉 LVGL 过了 5ms
      └─ lv_timer_handler()        LVGL 渲染一帧 → disp_flush → 写入共享内存
```

---

## 显示渲染管线

```
LVGL 渲染引擎
    │ lv_timer_handler() 触发绘制
    ▼
disp_flush() 回调
    │ 将 LVGL 8bit 像素转换为 4bpp 灰度
    │ 每 2 个像素打包成 1 字节（高4位 + 低4位）
    │ 写入 shm_buf（共享内存帧缓冲区）
    ▼
ai_display_commit_frame()
    │ 通知 display-service 有新帧
    ▼
display-service
    │ 将共享内存数据写入 JBD013 硬件
    ▼
光波导显示
```

像素打包格式：
```
源: pixel[0]=0xA0, pixel[1]=0x50
     ↓ 取高 4 位
打包: byte = 0xA0 | (0x50 >> 4) = 0xA5
```

帧缓冲区大小：640 × 480 ÷ 2 = 153,600 字节（每行 320 字节）

---

## GPIO 按键系统

### 硬件

眼镜镜腿上有两个物理按键：

| GPIO 编号 | 宏定义 | 功能 | 物理位置 |
|-----------|--------|------|---------|
| GPIO 0 | `GPIO_PAGE` | 翻页键 | 镜腿侧面 |
| GPIO 75 | `GPIO_CONFIRM` | 确认键 | 镜腿侧面 |

### 事件流

```
用手按下镜腿按键
    ↓
ai-core (GPIO Manager) 检测到电平变化
    ↓ 通过共享内存 + Unix Socket 广播
gpio_hub_callback() 被触发（独立线程）
    ↓ 设置 pending_gpio_event + ui_update_pending 标志
主循环检测到标志
    ↓
handle_page_key() 或 handle_confirm_key()
    ↓
更新 UI 状态 → LVGL 重新渲染 → 光波导刷新
```

### 事件类型

| 事件 | 说明 | 当前处理方式 |
|------|------|------------|
| `GPIO_EVENT_PRESS` | 按键按下 | 触发 UI 操作 |
| `GPIO_EVENT_RELEASE` | 按键释放 | 未处理（可用于长按检测） |
| `GPIO_EVENT_ERROR` | 错误 | 未处理 |

### 线程安全

GPIO 回调在独立线程中执行，通过以下机制保证线程安全：
- `pthread_mutex_t ui_mutex`：保护 UI 状态变量
- `volatile` 标志 `ui_update_pending` 和 `pending_gpio_event`：回调线程写入，主循环读取
- UI 操作全部在主循环中执行（LVGL 非线程安全）

---

## UI 界面与状态机

### 状态定义

```c
typedef enum {
    STATE_HOME,          // 首页
    STATE_SUB_MENU,      // 子菜单
    STATE_TELEPROMPTER,  // 提词器（保留，无入口）
} ui_state_t;
```

### 状态转换图

```
                    ┌──────────────────────────────┐
                    │                              │
                    ▼                              │
              STATE_HOME                           │
         ┌──────────────────┐                      │
         │  [拍照] [录像] [更多]  │                      │
         │   ↑选中框              │                      │
         └──────────────────┘                      │
            │           │                          │
  翻页键：循环切换    确认键：                        │
  拍照→录像→更多       │                            │
      (循环)          ├─ 选中"拍照" → 打印日志        │
                     ├─ 选中"录像" → 打印日志        │
                     └─ 选中"更多" → 进入子菜单       │
                                    │              │
                                    ▼              │
                              STATE_SUB_MENU       │
                         ┌──────────────────┐      │
                         │  接听  休眠  个性化  │      │
                         │  位置  电话/拨通  显示图│      │
                         │  姿态  退出  音量    │      │
                         └──────────────────┘      │
                                    │              │
                              确认键：返回首页 ──────┘
```

### 首页 (STATE_HOME)

首页容器 `ui_VideoContainer`（640×480 全屏），三列布局：

#### 左列 — 拍照
- **图标**：圆形（直径42，空心白边）+ 圆角矩形（81×81，空心白边）+ 实心圆点（直径6）
- **文字**：`"拍照"`，位置 (50, 239)，48号阿里巴巴字体
- **选中框位置**：(5, 110)，尺寸 173×200

#### 中列 — 录像
- **图标**：80×80 圆角矩形（空心白边）+ 三条白色线段组成摄像机形状
- **文字**：`"录像"`，位置 (276, 239)
- **选中框位置**：(233, 110)

#### 右列 — 更多
- **图标**：十字形（8×60 竖条 + 60×8 横条，空心白边），加号形状
- **文字**：`"更多"`，位置 (500, 239)
- **选中框位置**：(461, 110)

#### 选中框
- `ui_SelectionRect`：173×200 白色透明边框矩形，12px 圆角
- 位于最底层（不遮挡图标和文字）
- 随 `current_menu_index` 在三个位置之间移动

#### 按键操作

| 按键 | 动作 |
|------|------|
| **翻页键** (GPIO 0) | `current_menu_index` 循环 +1（0→1→2→0），移动选中框 |
| **确认键** (GPIO 75) | 根据当前选中项执行操作 |

确认键行为：
- `HOME_ITEM_CAMERA` (0)：仅打印 `[Launcher] ACTION: Camera`（**无实际功能**）
- `HOME_ITEM_RECORD` (1)：仅打印 `[Launcher] ACTION: Record`（**无实际功能**）
- `HOME_ITEM_MORE` (2)：切换到 `STATE_SUB_MENU`

### 子菜单 (STATE_SUB_MENU)

子菜单容器 `ui_subMenu`（640×450），黑色背景，9 个文字标签分三行排列：

| 行 | Y 坐标 | 标签内容 |
|----|--------|---------|
| 第一行 | Y=124 | 接听、休眠、个性化 |
| 第二行 | Y=239 | 位置、电话/拨通、显示图 |
| 第三行 | Y=360 | 姿态、退出、音量 |

底部有一个白色矩形指示器 `ui_SubMenu_Rect`（58×2，位置 69,174）。

#### 按键操作

| 按键 | 动作 |
|------|------|
| **翻页键** (GPIO 0) | 无响应（未实现子菜单导航） |
| **确认键** (GPIO 75) | 直接返回 `STATE_HOME` |

**所有子菜单项目前均无实际功能逻辑。**

### 隐藏的 UI 元素

| 控件 | 说明 |
|------|------|
| `ui_VideoRecordingContainer` | 270×480 居中边框，默认隐藏，未被任何逻辑激活 |
| `ui_Navgation` 屏幕 | 导航屏，有一个空文字标签，从未被加载 |
| `STATE_TELEPROMPTER` | 提词器状态，代码中保留但无入口可进入 |

---

## 功能实现状态

| 功能 | 状态 | 说明 |
|------|------|------|
| 光波导显示 | **已实现** | LVGL → 4bpp → 共享内存 → display-service |
| GPIO 按键响应 | **已实现** | 翻页/确认键，异步回调 + 主循环处理 |
| 首页菜单导航 | **已实现** | 三项循环切换 + 选中框高亮 |
| 首页→子菜单跳转 | **已实现** | 确认键选"更多"进入，确认键返回 |
| 屏幕省电超时 | **已实现** | `--powersave_timeout` 参数 |
| 拍照 | **未实现** | 仅打印日志，需接入 `ai_camera` SDK |
| 录像 | **未实现** | 仅打印日志，需接入 `ai_camera` SDK |
| 子菜单各项功能 | **未实现** | 接听/休眠/个性化/位置/电话/显示图/姿态/音量均无逻辑 |
| 子菜单导航 | **未实现** | 翻页键在子菜单中无响应 |
| 蓝牙数据接收 | **未实现** | 需新增蓝牙 RFCOMM 模块 |
| 麦克风采集 | **未实现** | 需接入 `ai_audio` SDK |
| 喇叭播放 | **未实现** | 需接入 `ai_audio` SDK |
| 长按检测 | **未实现** | 当前仅处理 PRESS 事件，可加 RELEASE 实现 |

---

## 依赖关系

```
my-app
  ├── libai_glass_sdk.a (静态链接)
  │     ├── ai_display  → 通过共享内存连接 display-service
  │     └── ai_gpio     → 通过共享内存+Socket 连接 ai-core GPIO Hub
  ├── libpthread (动态链接) → 多线程支持
  ├── librt (动态链接) → POSIX 共享内存
  └── libm (动态链接) → 数学库

系统服务依赖：
  display-service  → 必须运行，否则无法显示
  ai-core + guard  → 必须运行，否则 GPIO 按键无响应
```

---

## 开发指引

### 修改 UI 界面

UI 控件定义在 `ui/screens/ui_Screen1.c`，使用 LVGL API：
- 创建控件：`lv_obj_create()`、`lv_label_create()`、`lv_line_create()`
- 设置位置/大小：`lv_obj_set_pos()`、`lv_obj_set_size()`
- 设置样式：`lv_obj_set_style_*()` 系列函数
- 显示/隐藏：`lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN)` / `lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN)`

### 添加新功能

1. 在 `main.c` 的状态枚举中添加新状态
2. 在 `ui_Screen1.c` 中创建对应的 UI 控件
3. 在 `handle_page_key()` / `handle_confirm_key()` 中添加操作逻辑
4. 在 `switch_to_state()` 中处理新状态的界面切换

### 添加新的 SDK 功能

参考 SDK 头文件（`../../SDK/ai_glass_sdk/include/`）：
- 相机：`#include "ai_camera.h"`
- 音频：`#include "ai_audio.h"`
- 文本事件：`#include "ai_text_event.h"`

在 Makefile 中 SDK 已经链接，直接 `#include` 即可使用。

### 编译-部署-测试循环

```bash
# 修改代码后
make clean && make && make deploy

# 在眼镜上重启 my-app
adb shell killall my-app
adb shell /oem/usr/bin/my-app &

# 查看运行日志
adb shell /oem/usr/bin/my-app    # 前台运行，可看到所有 printf 输出
```

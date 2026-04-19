# my-app 踩坑笔记（易复发问题清单）

本文件汇总本项目在替换闭源 ai-core / 对接 lumina 三服务过程中踩过的坑，
主要用于下次遇到类似现象时快速定位。按子系统分组，每条 = **现象 → 根因 → 修法/规避**。

---

## 1. 音频（ALSA / mic_pump）

### 1.1 mic 开了关不掉，`pthread_join` 卡死
- 现象：`mic_pump_stop()` 不返回；dispatcher 线程随后卡在 `snd_pcm_open`，
  触摸板 / 电源键 press 日志正常但 UI 无反应。
- 根因：RK 内核里 `SNDRV_PCM_IOCTL_DROP` 不能可靠唤醒阻塞中的 `READI_FRAMES`。
  pump 线程一直卡在 `__snd_pcm_lib_xfer`，PCM 设备未释放，下一次 `snd_pcm_open`
  就在内核里 hang 住。
- 修法：`core/mic_capture.c`
  1. `SIGUSR1` 装一个 **不带 `SA_RESTART`** 的 no-op handler；
  2. pump 线程自己 `pthread_sigmask(SIG_UNBLOCK, SIGUSR1)`，其他线程在 `main()`
     里整体 `SIG_BLOCK` 掉 `SIGUSR1`，避免误收；
  3. `mic_pump_stop` 里置 `stopping=1` + `ioctl(DROP)` 之后，**循环 `pthread_kill(tid, SIGUSR1)` + `nanosleep(10ms)`** 若干次，
     强制让 `ioctl` 以 `-EINTR` 返回，再 `pthread_join`。
- 规避原则：任何会长期阻塞在 kernel 的线程，一定要有基于信号的退出兜底，
  不要只依赖"设置 running=0 + 自然退出"。

### 1.2 双重打开 PCM 设备导致 dispatcher 死锁
- 现象：同 1.1，但原因在业务侧。
- 根因：`g_mic` 还非空时再次 `mic_pump_start()`，两个线程抢同一 `/dev/snd/pcmC0D0c`；
  或者 server 发来的 `done` 把状态改回 `ST_IDLE`，用户以为"按一下关麦"却走了"新开一轮"分支，
  又 open 一次被占用的 PCM → hang。
- 修法：
  1. 所有 `mic_pump_start()` 之前加兜底 `if (g_mic) stop_mic();`。
  2. Server 推送的 `done / interrupted` **只在 `ST_WAIT` 时才回 `ST_IDLE`**，
     `ST_REC` 期间坚决不动状态和 mic（尤其 `realtime_translate` 是连续识别，每句都会 done）。

### 1.3 aplay 缺失 / 播放失败
- 规避：所有播放走 `core/speaker` 内置 ALSA 写入，不要 `system("aplay ...")`。

---

## 2. 摄像头（rkipc / V4L2）

### 2.1 "拍照失败" 因为设备上没 `ffmpeg`
- 修法：用 `libjpeg` 直接把 NV12/NV21 帧在进程内编成 JPEG，不外调工具链。

### 2.2 图像偏绿、偏暗
- 根因：RK cif 默认输出实际是 **NV21**（Cr/Cb 顺序与 NV12 相反），硬套 NV12 → 绿屏。
- 修法：`nv12_to_jpeg()` 里按 NV21 顺序取 UV；`CAM_EXPOSURE` / `CAM_GAIN` 调高一档。
- 规避：接新 sensor 先 `v4l2-ctl --list-formats-ext` 确认实际 fourcc。

---

## 3. GPIO / 输入

### 3.1 自写 sysfs 轮询漏事件
- 规避：功能键统一走 `/dev/input/eventX`（KEY_POWER 116 等），
  由 `core/power_key` 转成 `gpio/press` 发布到事件总线；
  不要同一条物理键 sysfs + input 双路处理。

### 3.2 按键"有 log 但无反应"
- 先区分是哪层日志：
  - 只有 `[power_key] PRESS` → reader 线程 OK，**dispatcher 线程卡住**。
    99% 是 mic 相关死锁（见 1.1 / 1.2），用 `cat /proc/<pid>/task/<tid>/wchan`、
    `stack` 定位。
  - 同时有 `[app] GPIOx PRESS` → dispatcher 到了 app，检查 app 侧状态机分支。

### 3.3 同一个按键 / 多种语义
- 触摸板2 曾既当"进入功能"又当"收音 toggle"，很容易和物理电源键冲突。
- 现在约定：**收音全部由电源键控制，触摸板2 = 首页单击进入 / 功能中单击退出**。
  以后新增按键动作先回来审一次这张表。

---

## 4. WebSocket / mongoose

### 4.1 整机卡顿、感觉 WS 阻塞按键
- 根因：mongoose 默认 `MG_LL_DEBUG` 日志量巨大，stdout 被刷爆。
- 修法：`ws_client_open` 里 `mg_log_set(MG_LL_ERROR)`。
- 规避：任何引入的 C 库先确认默认日志级别。

### 4.2 `ws_client_is_open` 返回 false 后按键失效
- 根因：lumina 服务常在 `done` 后主动关 WS；客户端没重连逻辑就挂在"WS 未就绪"。
- 修法：每个 app 里写一个 `ensure_ws_open()`，进入功能 + 每次用户触发前都调一下，
  握手超时 2s，超时给提示。
- 规避：**WS 生命周期 = 功能生命周期**（enter 建连、exit 断连），不要全局常驻。

### 4.3 tx 队列锁范围
- `tx_drain_and_send` 里的 mutex **只保护 pop**，`mg_ws_send` 必须放在锁外，
  否则 pump 线程的 `tx_push` 会和网络 I/O 争锁造成卡顿。

---

## 5. 进程 / 启动

### 5.1 adb shell 退出后 my-app 被 SIGHUP 杀掉
- 修法：`app_switch.sh` 用 `nohup ... </dev/null >log 2>&1 &`；不要依赖 `setsid`
  （有的 rootfs 没这个命令）。

### 5.2 两个启动程序同时被自动拉起
- 规避：`/etc/init.d/Sxx_*` 里只保留二选一；修改后 `sync && reboot` 验证。

---

## 6. LVGL / 显示

### 6.1 my-app 白屏 / 只有两条横线
- 根因：背光 / SPI 初始化 + LVGL theme 与 launcher-app 不一致。
- 规避：新功能页先从现有能正常显示的页面复制一份，再逐项替换内容；
  显示异常时先 `diff` 这部分和 launcher-app。

### 6.2 label 居中/换行不符合预期
- 规避：功能页的 label **显式 `lv_obj_set_style_text_align(LV_TEXT_ALIGN_LEFT, 0)`**，
  不要假设默认对齐；`LV_LABEL_LONG_WRAP` + 固定宽度一起设。

### 6.3 跨线程操作 LVGL 控件
- 所有非主线程对 LVGL 的读写都必须 `app_ui_lock() / unlock()` 包起来
  （包括 WS 回调线程、事件总线 dispatcher、mic callback）。

---

## 7. 信号 / 多线程通用

- `main()` 一开始就 `pthread_sigmask(SIG_BLOCK, {SIGUSR1, ...})`，
  后续 `pthread_create` 的线程默认继承。需要该信号的线程自己 `UNBLOCK`。
- 任何 "设置 volatile 标志让循环自己退出" 的退出方案，
  如果循环里有可能阻塞在 kernel，就**必须**配一个信号兜底。
- 不要在信号 handler 里做除"置标志位"以外的事。

---

## 8. 调试小抄

```sh
# 找 my-app 进程
adb shell "pidof my-app || ps | grep my-app"

# 看某线程在哪阻塞（最常用）
adb shell "cat /proc/<pid>/task/<tid>/wchan; echo; cat /proc/<pid>/task/<tid>/stack"

# 列出所有线程
adb shell "ls /proc/<pid>/task/ && for t in \$(ls /proc/<pid>/task/); do
  echo === \$t \$(cat /proc/<pid>/task/\$t/comm); cat /proc/<pid>/task/\$t/wchan; echo; done"

# 重启 my-app（不依赖 init 脚本）
adb shell "pkill -9 my-app; sleep 1; nohup /oem/usr/bin/my-app </dev/null >/tmp/myapp.log 2>&1 &"
adb shell "tail -f /tmp/myapp.log"
```

---

## 9. 改代码前自查 checklist

- [ ] 新起的线程是否在 kernel 里可能阻塞？是 → 准备好信号中断路径。
- [ ] 新加状态变更是否会把 mic/相机等独占资源留在打开状态？是 → 先 stop 再 start，加兜底。
- [ ] 新增按键/触摸动作是否和现有键位冲突？是 → 同步更新本文第 3.3 节。
- [ ] 新接入网络服务：WS 生命周期绑定功能；握手超时；重连；日志级别。
- [ ] 新加 LVGL 控件：显式设 align / wrap / font；跨线程访问加锁。

#!/bin/sh
# app_switch.sh - 在 my-app 和 launcher-app（旧闭源栈）之间二选一启动
#
# 用法:
#   app_switch.sh myapp [-f]     # 启动自研 my-app（纯开源路径，不依赖 guard/ai-core）
#   app_switch.sh launcher [-f]  # 启动旧栈: guard -> ai-core -> display-service -> launcher-app
#   app_switch.sh stop           # 关闭当前在跑的一切应用
#   app_switch.sh restart        # 按当前在跑的栈重启（只认 my-app 或 launcher 栈）
#   app_switch.sh restart myapp    # 停掉后重新拉起 my-app
#   app_switch.sh restart launcher  # 停掉后重新拉起旧栈
#   app_switch.sh status         # 查看当前哪些进程在跑
#   app_switch.sh log [myapp|launcher]  # 跟随对应 log (tail -F)
#   app_switch.sh autostart on   # 开机自动拉起 my-app
#   app_switch.sh autostart off  # 关闭开机自启
#   app_switch.sh autostart      # 查看当前自启状态
#
# 加 -f 会在启动完成后自动 tail -F 对应 log（Ctrl-C 只退出 tail，不会杀 app）
#   对 myapp/launcher/restart 均适用，例如: app_switch.sh restart myapp -f
# 放在设备 /oem/usr/bin/app_switch.sh 下。
#
# 开机自启机制：/etc/init.d/S60_app_launcher 会在开机时检查
# $AUTOSTART_FLAG（/oem/etc/myapp_autostart），存在就后台拉起 my-app。
# 这里 autostart on/off 其实只是 touch / rm 那个标志文件，持久化随 /oem 分区。

set -u
export LD_LIBRARY_PATH=/oem/usr/lib:${LD_LIBRARY_PATH:-}

MYAPP_BIN=/oem/usr/bin/my-app
GUARD_BIN=/oem/usr/bin/guard
DISP_BIN=/oem/usr/bin/display-service
LAUNCHER_BIN=/oem/usr/bin/launcher-app

LOG_DIR=/tmp
MYAPP_LOG=$LOG_DIR/my-app.log
LAUNCHER_LOG=$LOG_DIR/launcher-app.log

# 自启标志：存在 → 开机拉起 my-app；不存在 → 开机不动
AUTOSTART_FLAG=/oem/etc/myapp_autostart

is_running() {
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        [ "$(cat "$p/comm" 2>/dev/null)" = "$1" ] && return 0
    done
    return 1
}

stop_all() {
    echo "[app_switch] stopping any running apps..."
    killall my-app launcher-app display-service ai-core guard 2>/dev/null
    # 给点时间让 guard 释放 SPI/audio 等硬件
    sleep 1
    killall -9 my-app launcher-app display-service ai-core guard 2>/dev/null
    sleep 1
}

status() {
    for name in my-app launcher-app display-service ai-core guard; do
        if is_running "$name"; then
            printf "  %-18s RUNNING\n" "$name"
        else
            printf "  %-18s stopped\n" "$name"
        fi
    done
}

# 重启：与 start_* 一样会先 stop_all，再按栈拉起。$1 = myapp|launcher|空
# 空参数时：在跑 my-app 则重拉 my-app；否则若 launcher/guard 在跑则重拉旧栈
_restart_follow=
do_restart() {
    _restart_follow=
    case "$1" in
        myapp)    start_myapp ;;
        launcher) start_launcher ;;
        "")
            if is_running my-app; then
                echo "[app_switch] restart: 检测到 my-app 在跑，正在重启..."
                _restart_follow=myapp
                start_myapp
            elif is_running launcher-app || is_running guard; then
                echo "[app_switch] restart: 检测到 launcher 栈在跑，正在重启..."
                _restart_follow=launcher
                start_launcher
            else
                echo "[app_switch] restart: 未检测到在跑的应用，请显式指定:"
                echo "             $0 restart myapp   或   $0 restart launcher"
                exit 1
            fi
            ;;
        *)
            echo "[app_switch] restart: 未知目标 '$1'（请用 myapp 或 launcher）"
            exit 1
            ;;
    esac
}

start_myapp() {
    [ -x "$MYAPP_BIN" ] || { echo "[app_switch] $MYAPP_BIN missing or not executable"; exit 1; }
    stop_all
    echo "[app_switch] starting my-app ..."
    # nohup + </dev/null + &: 让 my-app 脱离当前 shell 的 controlling terminal，
    # 否则 adb 单条命令执行完退出时，my-app 会随 shell 收到 SIGHUP 被杀。
    # （设备上无 setsid，改用 nohup；它会忽略 SIGHUP 并重定向 stdout）
    nohup "$MYAPP_BIN" </dev/null > "$MYAPP_LOG" 2>&1 &
    sleep 2
    if is_running my-app; then
        echo "[app_switch] my-app PID=$(pgrep my-app 2>/dev/null)"
        echo "[app_switch] log: $MYAPP_LOG"
        echo "----- $MYAPP_LOG (last 20 lines) -----"
        tail -20 "$MYAPP_LOG"
        echo "---------------------------------------"
    else
        echo "[app_switch] my-app exited early, last log:"
        tail -30 "$MYAPP_LOG"
        exit 1
    fi
}

start_launcher() {
    for b in "$GUARD_BIN" "$DISP_BIN" "$LAUNCHER_BIN"; do
        [ -x "$b" ] || { echo "[app_switch] missing $b"; exit 1; }
    done
    stop_all
    echo "[app_switch] starting guard (will spawn ai-core) ..."
    nohup "$GUARD_BIN" </dev/null >/tmp/guard.log 2>&1 &
    # 等 guard + ai-core 就绪（最多 30 秒）
    i=0
    while [ $i -lt 30 ]; do
        if is_running guard && is_running ai-core; then
            echo "[app_switch]   guard + ai-core ready"
            break
        fi
        sleep 1
        i=$((i+1))
    done

    echo "[app_switch] starting display-service ..."
    nohup "$DISP_BIN" </dev/null >/tmp/display-service.log 2>&1 &
    sleep 2

    echo "[app_switch] starting launcher-app ..."
    nohup "$LAUNCHER_BIN" </dev/null > "$LAUNCHER_LOG" 2>&1 &
    sleep 2
    if is_running launcher-app; then
        echo "[app_switch] launcher-app started, log: $LAUNCHER_LOG"
        echo "----- $LAUNCHER_LOG (last 20 lines) -----"
        tail -20 "$LAUNCHER_LOG"
        echo "------------------------------------------"
    else
        echo "[app_switch] launcher-app exited early, last log:"
        tail -30 "$LAUNCHER_LOG"
        exit 1
    fi
}

autostart_cmd() {
    # $1 = on|off|""
    case "${1:-}" in
        on)
            mkdir -p "$(dirname "$AUTOSTART_FLAG")"
            : > "$AUTOSTART_FLAG"   # touch，内容留空，靠文件存在与否判断
            sync
            echo "[app_switch] autostart: ENABLED  (flag=$AUTOSTART_FLAG)"
            echo "             下次开机会自动拉起 my-app"
            ;;
        off)
            rm -f "$AUTOSTART_FLAG"
            sync
            echo "[app_switch] autostart: DISABLED (flag removed)"
            echo "             下次开机不会自动拉起"
            ;;
        ""|status)
            if [ -f "$AUTOSTART_FLAG" ]; then
                echo "[app_switch] autostart: ENABLED  ($AUTOSTART_FLAG exists)"
            else
                echo "[app_switch] autostart: DISABLED ($AUTOSTART_FLAG not found)"
            fi
            ;;
        *)
            echo "[app_switch] autostart: unknown arg '$1' (use on|off|status)"
            exit 1 ;;
    esac
}

follow_log() {
    # $1 = myapp|launcher
    case "$1" in
        myapp|"")  f=$MYAPP_LOG ;;
        launcher)  f=$LAUNCHER_LOG ;;
        *)         echo "[app_switch] unknown log target: $1"; exit 1 ;;
    esac
    [ -f "$f" ] || { echo "[app_switch] $f not found"; exit 1; }
    echo "[app_switch] tail -F $f  (Ctrl-C 退出 tail，不会影响 app)"
    exec tail -F "$f"
}

FOLLOW=0
if [ "${1:-}" = "restart" ]; then
    [ "${3:-}" = "-f" ] && FOLLOW=1
else
    [ "${2:-}" = "-f" ] && FOLLOW=1
fi

case "${1:-}" in
    myapp)       start_myapp ;;
    launcher)    start_launcher ;;
    restart)     do_restart "${2:-}" ;;
    stop)        stop_all; status; exit 0 ;;
    status|"")   status; autostart_cmd status; exit 0 ;;
    log)         follow_log "${2:-myapp}" ;;
    autostart)   autostart_cmd "${2:-}"; exit 0 ;;
    *)
        echo "Usage: $0 {myapp|launcher|restart|stop|status|log|autostart} [-f|on|off]"
        exit 1 ;;
esac

echo ""
echo "[app_switch] current status:"
status

if [ "$FOLLOW" = "1" ]; then
    case "$1" in
        myapp)    follow_log myapp ;;
        launcher) follow_log launcher ;;
        restart)
            rt="${2:-}"
            [ -n "$_restart_follow" ] && rt="$_restart_follow"
            case "$rt" in
                myapp)    follow_log myapp ;;
                launcher) follow_log launcher ;;
                *)
                    echo "[app_switch] -f 需配合: $0 restart myapp|launcher" >&2
                    exit 1
                    ;;
            esac
            ;;
    esac
fi

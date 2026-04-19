##
## my-app Makefile
##
## 已移除对闭源 libai_glass_sdk.a 的链接依赖。
## 所有功能通过 my-app/core/ 下的自研模块直接访问硬件。
##

DEV_ENV_ROOT    := ../../..
TOOLCHAIN_PATH  := $(DEV_ENV_ROOT)/tools/linux/toolchain/arm-rockchip831-linux-uclibcgnueabihf/bin
CC              := $(TOOLCHAIN_PATH)/arm-rockchip831-linux-uclibcgnueabihf-gcc

# ---------- 构建目录 ----------
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
BIN_DIR   := $(BUILD_DIR)/bin

TARGET     := my-app
TARGET_BIN := $(BIN_DIR)/$(TARGET)

# ---------- 源文件 ----------
MAIN_SRC   := main.c
CORE_SRC   := \
    core/event_bus.c      \
    core/gpio_hub.c       \
    core/audio_player.c   \
    core/tts_player.c     \
    core/mic_capture.c    \
    core/camera.c         \
    core/wifi.c           \
    core/config.c         \
    core/power_key.c      \
    core/net/ws_client.c  \
    core/net/base64.c     \
    core/display/waveguide.c \
    core/display/hal_driver.c \
    core/display/jbd013_api.c

APP_SRC    := \
    app/app_common.c      \
    app/app_vlm.c         \
    app/app_translate.c   \
    app/app_homework.c    \
    app/hw_tree.c

THIRD_SRC  := \
    third_party/mongoose/mongoose.c \
    third_party/cJSON/cJSON.c

ALL_SRC    := $(MAIN_SRC) $(CORE_SRC) $(APP_SRC) $(THIRD_SRC)
ALL_OBJS   := $(patsubst %.c, $(OBJ_DIR)/%.o, $(ALL_SRC))

# ---------- 编译选项 ----------
CFLAGS  := -Wall -Wextra -O2 -D_GNU_SOURCE
CFLAGS  += -I. -Icore -Icore/display -Icore/net -Iapp
CFLAGS  += -Ithird_party/mongoose -Ithird_party/cJSON
# mongoose: 关闭 TLS / 文件系统特性，纯 ws:// 客户端
CFLAGS  += -DMG_ENABLE_OPENSSL=0 -DMG_ENABLE_MBEDTLS=0 -DMG_ENABLE_FS=0 -DMG_TLS=0

# libjpeg（用于 NV12 -> JPEG 软编码，设备无 ffmpeg）
LIBJPEG_ROOT ?= $(DEV_ENV_ROOT)/output/out/app_out
CFLAGS  += -I$(LIBJPEG_ROOT)/include/libjpeg9
LDFLAGS := -L$(LIBJPEG_ROOT)/lib -ljpeg -lpthread -lrt -lm

# LVGL UI (main.c 依赖；如需裸运行可 `make ENABLE_LVGL=0`，但将无法显示 UI)
ENABLE_LVGL ?= 1
ifeq ($(ENABLE_LVGL),1)
    LVGL_DIR_NAME ?= lvgl
    LVGL_DIR      ?= ../../third_party
    LVGL_PATH     := $(LVGL_DIR)/lvgl
    CFLAGS += -I$(LVGL_DIR) -DLV_CONF_INCLUDE_SIMPLE -DENABLE_LVGL=1
    include $(LVGL_PATH)/lvgl.mk
    UI_DIR  := ui
    UI_SRC  := $(shell find $(UI_DIR) -type f -name '*.c' 2>/dev/null)
    ALL_OBJS += $(patsubst %.c, $(OBJ_DIR)/%.o, $(UI_SRC))
    LVGL_OBJ := $(addprefix $(OBJ_DIR)/lvgl/, $(CSRCS:.c=.o))
    ALL_OBJS += $(LVGL_OBJ)
endif

.PHONY: all clean deploy deploy-conf deploy-script debug_vars
all: $(TARGET_BIN)

debug_vars:
	@echo "SRC:  $(ALL_SRC)"
	@echo "OBJS: $(ALL_OBJS)"

$(TARGET_BIN): $(ALL_OBJS)
	@mkdir -p $(dir $@)
	@echo "LINK  $@"
	@$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# LVGL 第三方源
$(OBJ_DIR)/lvgl/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

deploy: $(TARGET_BIN)
	adb push $(TARGET_BIN) /oem/usr/bin/$(TARGET)
	adb shell chmod +x /oem/usr/bin/$(TARGET)
	@echo "Deployed to /oem/usr/bin/$(TARGET)"

# 把运行时配置 (WiFi / 算法服务) 推到眼镜
# 首次部署时必须执行一次；之后只有改配置时再执行
deploy-conf:
	adb shell mkdir -p /oem/etc
	adb push scripts/myapp.conf.sample /oem/etc/myapp.conf
	@echo "Deployed config to /oem/etc/myapp.conf"

# 把启停脚本 app_switch.sh 推到眼镜（一次就够，除非脚本改了）
deploy-script:
	adb push scripts/app_switch.sh /oem/usr/bin/app_switch.sh
	adb shell chmod +x /oem/usr/bin/app_switch.sh
	@echo "Deployed script to /oem/usr/bin/app_switch.sh"

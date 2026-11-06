CC = gcc
PKGS = gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 libmicrohttpd json-glib-1.0
BUILD_DIR ?= ../../../temp/camera-web-build
CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
CFLAGS += -std=c11 -O2 -g -Wall -Wextra -Wpedantic -pthread
LDLIBS += $(shell pkg-config --libs $(PKGS)) -pthread -lm

.PHONY: all probe motor-check distance-check
all: $(BUILD_DIR)/opto-camera

$(BUILD_DIR)/opto-camera: camera_web.c image_quality.c image_quality.h image_controls.c image_controls.h rkaiq_client.c rkaiq_client.h motor_control.c motor_control.h Makefile
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) camera_web.c image_quality.c image_controls.c rkaiq_client.c motor_control.c -o $@ $(LDLIBS)

# 只读参数诊断；与网页服务分开构建，只查询当前参数，不写入设置。
probe: $(BUILD_DIR)/image-controls-probe

$(BUILD_DIR)/image-controls-probe: diagnostics/image_controls_probe.c rkaiq_client.c rkaiq_client.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(shell pkg-config --cflags json-glib-1.0) diagnostics/image_controls_probe.c rkaiq_client.c -o $@ $(shell pkg-config --libs json-glib-1.0) -pthread

# 仅在RK/Linux虚拟串口核对字节与计时，不打开真实电机串口。
motor-check: $(BUILD_DIR)/motor-control-check

$(BUILD_DIR)/motor-control-check: diagnostics/motor_control_check.c motor_control.c motor_control.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(shell pkg-config --cflags json-glib-1.0) diagnostics/motor_control_check.c motor_control.c -o $@ $(shell pkg-config --libs json-glib-1.0) -pthread -lutil -lm

# 定距离检查也只接Linux虚拟串口，检查累计计数、减速、停车与异常输入。
distance-check: $(BUILD_DIR)/motor-distance-check

$(BUILD_DIR)/motor-distance-check: diagnostics/motor_distance_check.c motor_control.c motor_control.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(shell pkg-config --cflags json-glib-1.0) diagnostics/motor_distance_check.c motor_control.c -o $@ $(shell pkg-config --libs json-glib-1.0) -pthread -lutil -lm

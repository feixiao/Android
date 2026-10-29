.PHONY: all help build build-release install reinstall run stop restart logcat clean test uninstall devices info

APK_DEBUG = app/build/outputs/apk/debug/app-debug.apk
APK_RELEASE = app/build/outputs/apk/release/app-release.apk
PACKAGE_NAME = com.ai.audioagent
MAIN_ACTIVITY = com.ai.audioagent/.MainActivity
GRADLEW = ./gradlew

# Default target
all: help

help:
	@echo "================================================================="
	@echo "   Android AI Audio Agent Demo - Makefile 指南"
	@echo "================================================================="
	@echo "  make install       - 编译并安装 Debug APK 到已连接的 Android 设备"
	@echo "  make run           - 安装并立即启动主界面 (MainActivity)"
	@echo "  make build         - 仅编译 Debug APK (assembleDebug)"
	@echo "  make build-release - 编译 Release APK (assembleRelease)"
	@echo "  make reinstall     - 先卸载再重新安装"
	@echo "  make stop          - 强行停止应用进程"
	@echo "  make restart       - 重启应用"
	@echo "  make uninstall     - 卸载应用 $(PACKAGE_NAME)"
	@echo "  make logcat        - 查看全双工 Agent 实时运行时 Logcat 日志"
	@echo "  make test          - 编译并在宿主机运行 C++ 核心单元冒烟测试"
	@echo "  make clean         - 清理 Gradle 构建产物与临时文件"
	@echo "  make devices       - 查看当前连接的 adb 设备列表"
	@echo "  make info          - 查看已连接设备的 CPU 架构与主频拓扑"
	@echo "================================================================="

build:
	$(GRADLEW) assembleDebug

build-release:
	$(GRADLEW) assembleRelease

install: build
	@echo "==> 正在安装 Debug APK 到设备..."
	adb install -r -t $(APK_DEBUG)
	@echo "==> 安装完成！"

reinstall: build
	@echo "==> 正在卸载旧版本..."
	-adb uninstall $(PACKAGE_NAME)
	@echo "==> 正在重新安装..."
	adb install -r -t $(APK_DEBUG)
	@echo "==> 安装完成！"

run: install
	@echo "==> 正在启动 $(MAIN_ACTIVITY)..."
	adb shell am start -n $(MAIN_ACTIVITY)

stop:
	@echo "==> 正在停止 $(PACKAGE_NAME)..."
	adb shell am force-stop $(PACKAGE_NAME)

restart: stop run

uninstall:
	@echo "==> 正在卸载 $(PACKAGE_NAME)..."
	adb uninstall $(PACKAGE_NAME)

logcat:
	@echo "==> 监听全双工语音 Agent 核心标签日志 (按 Ctrl+C 退出)..."
	adb logcat -v time -s AudioAgentNative:V AudioAgentManager:V AudioAgentService:V SystemToolExecutor:V MainActivity:V AndroidRuntime:E

test:
	@echo "==> 编译宿主机 C++17 单元测试..."
	cd app/src/main/cpp && clang++ -std=c++17 -O2 -Wall -Wextra \
		-I include \
		src/CpuAffinity.cpp \
		src/StreamingSentenceChunker.cpp \
		src/WatchdogAndRepetition.cpp \
		src/ThreeTierMemory.cpp \
		src/FullDuplexAudioIO.cpp \
		src/AudioAgentPipeline.cpp \
		src/MnnLlmEngine.cpp \
		src/test_main.cpp \
		-o audio_agent_test
	@echo "==> 执行自动化冒烟测试套件..."
	./app/src/main/cpp/audio_agent_test
	@rm -f app/src/main/cpp/audio_agent_test

clean:
	$(GRADLEW) clean
	rm -f app/src/main/cpp/audio_agent_test

devices:
	adb devices -l

info:
	@echo "==> 设备型号:"
	@adb shell getprop ro.product.model
	@echo "==> Android 系统版本:"
	@adb shell getprop ro.build.version.release
	@echo "==> CPU ABI 架构:"
	@adb shell getprop ro.product.cpu.abi
	@echo "==> 在线 CPU 核心:"
	@adb shell cat /sys/devices/system/cpu/online 2>/dev/null || true

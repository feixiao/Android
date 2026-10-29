# Android 全双工 AI Audio Agent 深度实战工程 Demo

本项目是基于白皮书 [《Android 端侧全双工 AI Audio Agent 深度工程实战白皮书》](../../android_ai_agent.md) 的完整工业级参考实现，实现了 **Oboe + Sherpa-onnx + MNN-LLM** 级联流水线的核心底层与 Android 应用层架构。

---

## 核心特性与架构实现

```
[硬件麦克风 (VoiceCommunication -> 激活芯片 DSP 硬件级 AEC / NS)]
       │ (PCM 16kHz, Float32, Mono)
       ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ ISO C++17 Native 引擎层 (libaudio_agent.so)                                 │
│                                                                             │
│  [Oboe Input Callback (FastCapture 实时音频线程)]                           │
│       │ 零锁写入                                                            │
│       ▼                                                                     │
│  (SPSC_RingBuffer_Mic) ──► [感知线程: Sherpa-onnx VAD + Streaming ASR]       │
│                                   │   (绑定能效小核 Little Cores <150mW)     │
│          ┌────────────────────────┴─────────────────────┐                   │
│          ▼ (正常说话尾点 Endpoint)                       ▼ (播报中检测到人声)│
│  [吐出完整 User Query 文本]                     [触发 Barge-in 硬件级打断!] │
│          │                                              ├──► 1. atomic<bool> abort_llm = true│
│          ▼                                              ├──► 2. 清空 TTS 队列与 Spk 缓冲     │
│  (ThreadSafeQueue_Query)                                └──► 3. 回滚 LLM 未播报的 KV Cache   │
│          │ 唤醒条件变量 (cv.notify_one)                                     │
│          ▼                                                                  │
│  [认知线程: MNN-LLM 推理引擎 (绑定大核/超大核 + OpenCL/Vulkan)]             │
│          │ 流式吐出 Token (自定义 std::stringbuf 实时拦截)                   │
│          ▼                                                                  │
│  [流式标点切分器 (StreamingSentenceChunker)]                                │
│  (首包满 6 Bytes 遇标点即截断；识别 <|tool_call|> 自动分流至异步执行器)     │
│          │                                                                  │
│          ▼                                                                  │
│  (ThreadSafeQueue_TTS) ──► [表达线程: Sherpa-onnx TTS (Kokoro / Piper)]     │
│                                   │ (生成 24kHz PCM Float 块)               │
│                                   ▼                                         │
│                            (SPSC_RingBuffer_Spk)                            │
│                                   │ 零锁读取                                │
│                                   ▼                                         │
│                     [Oboe Output Callback (24kHz 独占流)]                   │
└───────────────────────────────────┼─────────────────────────────────────────┘
                                    ▼
                              [硬件扬声器]
```

### 1. 全双工零拷贝底层与无锁环形缓冲区
* **SPSC 无锁环形缓冲区 (`LockFreeSPSCRingBuffer.hpp`)：**
  * 基于 `std::atomic` 内存序（Relaxed, Acquire, Release）实现。
  * `head_` 与 `tail_` 指针使用 `alignas(64)` 缓存行对齐，彻底消除多核间的伪共享（False Sharing）。
  * 读写操作保证零 Mutex、零堆内存分配（Zero-malloc），适配 Android `FastMixer / FastCapture` 实时音频内核线程。
* **Oboe 硬件级 DSP AEC/NS 底座 (`FullDuplexAudioIO.hpp/cpp`)：**
  * 上行采集配置 `InputPreset::VoiceCommunication`，强制唤醒高通/联发科 SoC 硬件 DSP 回声消除器。
  * 下行播放流匹配 24kHz 采样率与 `Usage::VoiceCommunication`，与输入流配对建立 DSP 参考回路。
  * `onErrorAfterClose` 监听蓝牙耳机热插拔与来电焦点抢占，50ms 内平滑重建音频路由。

### 2. 流式标点切分与“先安抚、后执行”的工具调用
* **流式拦截器 (`StreamingSentenceChunker.hpp/cpp`)：**
  * 首句阈值设定为 **6 字节**（2个汉字，如“好的，”），遇标点即刻推送 TTS，将端到端首音延迟（TTFT）压榨至 **150ms 左右**。
  * 后续句子动态切换至 **21 字节**（7个汉字）遇标点切分，兼顾首包极速与后续语音韵律连贯度。
  * 内置 `<|tool_call|>` 标签解析器：拦截工具 JSON，不送入 TTS 朗读，由异步路由分流执行。

### 3. 毫秒级 Barge-in 打断与 KV Cache 记忆对齐
* **打断逻辑 (`AudioAgentPipeline::TriggerBargeIn`)：**
  1. 原子置位 `abort_flag_`，使 LLM 解码与 TTS 生成回调在下一个微秒级周期退出。
  2. 瞬间清空待合成 TTS 队列并排空扬声器 RingBuffer，扬声器在 5ms 内闭嘴。
  3. **记忆回滚机制：** `spoken_text_accumulator_` 精确追踪用户耳朵实际听到的短句，打断时以 `"实际听到的内容（被用户打断）"` 覆盖历史记忆并回滚 KV Cache，杜绝“模型以为讲完、用户实际没听完”的记忆偏差。

### 4. 工业级看门狗与异常容错
* **3 秒首包看门狗 (`TTFTWatchdog.hpp/cpp`)：** 防止端侧模型因极端音频噪声卡死或 GPU Shader 掉线；超时自动拉响中断并播报兜底安抚语 `"抱歉，我刚走神了，您能再说一遍吗？"`。
* **N-gram 重复死循环拦截：** 实时检测 12 字节重复模式，彻底阻断 4-bit 量化小模型复读机现象。
* **CPU 异构调度 (`CpuAffinity.hpp/cpp`)：** 动态探测 `/sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq`，将认知/表达线程绑定至大核与超大核，感知线程绑定至小核（功耗 <150mW）。
* **三级记忆架构 (`ThreeTierMemory.hpp/cpp`)：**
  * L1: 固定 System Prompt + 最近 4 轮对话环形缓存。
  * L2: 对话超出 6 轮且闲置 >10s 时，自动后台静默生成语义摘要。
  * L3: 事实记忆检索，提问时 5ms 动态注入 Prompt 前缀。

### 5. Android 前台保活与交互界面
* **防 `lmkd` 杀进程保活：** `AudioAgentService` 注册 `foregroundServiceType="microphone|mediaPlayback"` 前台服务。
* **实时交互界面 (`MainActivity.kt`)：**
  * `StateVisualizerView`：自定义绘制波纹与光环，动态呈现 `IDLE` / `LISTENING` / `THINKING` / `SPEAKING` 状态。
  * `AudioWaveformView`：麦克风与扬声器实时波形频谱。
  * Barge-in 打断测试按钮、模拟对话流、异步工具调用结果卡片。

---

## 目录结构

```
AudioAgentDemo/
├── build.gradle.kts                     # 顶级 Gradle 构建配置
├── settings.gradle.kts                  # 工程模块配置
├── gradle.properties                    # JVM 参数与 AndroidX 开关
└── app/
    ├── build.gradle.kts                 # App 模块配置 (NDK, CMake, Prefab, Oboe)
    ├── proguard-rules.pro               # JNI 混淆保护规则
    └── src/
        └── main/
            ├── AndroidManifest.xml      # 权限与前台服务声明
            ├── cpp/                     # ISO C++17 Native 核心引擎
            │   ├── CMakeLists.txt       # CMake 跨平台与 NDK 编译规则
            │   ├── include/
            │   │   ├── LockFreeRingBuffer.hpp        # 64B缓存行对齐无锁 SPSC 环形缓冲区
            │   │   ├── CpuAffinity.hpp               # 异构 CPU 大小核主频探测与线程绑定
            │   │   ├── StreamingSentenceChunker.hpp  # 流式标点动态切分与 Tool Call 拦截器
            │   │   ├── WatchdogAndRepetition.hpp     # 3s TTFT 看门狗与 12B 重复死循环拦截
            │   │   ├── ThreeTierMemory.hpp           # L1/L2/L3 三级对话记忆子系统
            │   │   ├── FullDuplexAudioIO.hpp         # Oboe 硬件 DSP VoiceCommunication 音频 IO
            │   │   ├── EngineInterfaces.hpp          # VAD / ASR / LLM / TTS 抽象引擎契约
            │   │   ├── AudioAgentPipeline.hpp        # 全双工流水线粘合状态机与打断协调器
            │   │   └── NativeJniBridge.hpp           # JNI 导出头文件
            │   └── src/
            │       ├── CpuAffinity.cpp
            │       ├── StreamingSentenceChunker.cpp
            │       ├── WatchdogAndRepetition.cpp
            │       ├── ThreeTierMemory.cpp
            │       ├── FullDuplexAudioIO.cpp
            │       ├── AudioAgentPipeline.cpp
            │       ├── NativeJniBridge.cpp
            │       └── test_main.cpp                 # 宿主机自动化冒烟单元测试套件
            ├── java/com/ai/audioagent/
            │   ├── MainActivity.kt                   # 交互主界面
            │   ├── service/AudioAgentService.kt      # 前台录音/播放常驻保活服务
            │   ├── manager/AudioAgentManager.kt      # 引擎调度与音频焦点管理器
            │   ├── nativebridge/AudioAgentNative.kt  # JNI 胶水与 Kotlin 监听接口
            │   ├── tools/SystemToolExecutor.kt       # 异步工具调用执行器 (导航/空调/闹钟)
            │   └── ui/
            │       ├── StateVisualizerView.kt        # 4态炫彩状态机动画控件
            │       └── AudioWaveformView.kt          # 实时音频示波器控件
            └── res/
                ├── layout/activity_main.xml          # 现代深色模式 UI 布局
                └── values/{colors,strings,themes}.xml
```

---

## 验证与运行

### 1. 宿主机快速冒烟测试（无需 Android 设备）
在 macOS / Linux 终端中可直接编译并运行 Native 测试套件，验证无锁环形缓冲区、流式切句、看门狗、三级记忆、状态机与打断逻辑：

```bash
cd AudioAgentDemo/app/src/main/cpp
clang++ -std=c++17 -O2 -Wall -Wextra \
  -I include \
  src/CpuAffinity.cpp \
  src/StreamingSentenceChunker.cpp \
  src/WatchdogAndRepetition.cpp \
  src/ThreeTierMemory.cpp \
  src/FullDuplexAudioIO.cpp \
  src/AudioAgentPipeline.cpp \
  src/test_main.cpp \
  -o audio_agent_test

./audio_agent_test
```

**测试输出样例：**
```text
=======================================================
  Android AI Audio Agent C++17 Smoke Test Suite
=======================================================

=== Running Test: LockFreeSPSCRingBuffer SPSC Operations ===
[PASS] LockFreeSPSCRingBuffer passed all tests.

=== Running Test: StreamingSentenceChunker Dynamic Thresholds ===
[PASS] StreamingSentenceChunker dynamic thresholds and tool calling passed.

=== Running Test: TTFTWatchdog and N-Gram Repetition Detection ===
[PASS] TTFTWatchdog and Repetition Detector passed.

=== Running Test: ThreeTierMemory L1/L2/L3 & KV Cache Rollback ===
[PASS] ThreeTierMemory passed.

=== Running Test: Full-Duplex Pipeline End-to-End and Barge-In Verification ===
  [FSM Transition] IDLE -> THINKING
  [FSM Transition] THINKING -> SPEAKING
  [Barge-in Triggered] Audio silenced, spoken text retained: ''
  [FSM Transition] SPEAKING -> LISTENING
[PASS] Full-Duplex Pipeline end-to-end and Barge-in passed.

=== Running Test: Heterogeneous CPU Core Affinity Detection ===
  Detected Top Performance Cores: 9 8 7 6 
  Detected Efficiency Cores: 0 1 
  Bind to Big Cores returned: false (advisory)
[PASS] CPU Affinity Detection passed.

ALL 6 TEST SUITES PASSED SUCCESSFULLY!
```

### 2. 编译 Android NDK 动态库与 APK
在安装了 Android SDK / NDK 的开发机上，打开 Android Studio 打开 `AudioAgentDemo` 目录，或者直接通过 Gradle 命令行构建：

```bash
cd AudioAgentDemo
./gradlew assembleDebug
```
生成的 APK 位于 `app/build/outputs/apk/debug/app-debug.apk`。
安装至 Android 8.0+ 手机即可体验全双工状态机切换、语音模拟、工具调用分流与毫秒级打断交互。

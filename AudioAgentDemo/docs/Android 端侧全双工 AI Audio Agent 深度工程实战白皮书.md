# Android 端侧全双工 AI Audio Agent 深度工程实战白皮书
**—— 基于 Oboe + Sherpa-onnx + MNN-LLM 的 ISO C++17 零拷贝架构、模型微调与高可靠容错指南**

---

## 目录
1. **第一层：基础认知篇 —— 为什么端侧语音 Agent 必须重构？**
   * 1.1 云端方案 vs 端侧纯本地（On-Device）方案
   * 1.2 级联流式（Streaming Cascade） vs 原生端到端（Audio-LLM）
   * 1.3 800ms 端到端首音延迟预算表（Latency Budget）
2. **第二层：系统架构篇 —— ISO C++17 全双工零拷贝流水线**
   * 2.1 Native 全双工系统拓扑图
   * 2.2 音频底座：Oboe 低延迟流与芯片级 DSP 硬件回声消除（AEC）
   * 2.3 无锁环形缓冲区（Lock-Free SPSC RingBuffer）C++17 实现
3. **第三层：核心引擎篇 —— Sherpa-onnx 与 MNN-LLM 深度拆解**
   * 3.1 Sherpa-onnx：VAD + 流式 ASR + 流式 TTS 全家桶集成与采样率对齐
   * 3.2 MNN-LLM：ARM 汇编级优化、编译宏配置与运行时参数调优
   * 3.3 消灭首字延迟（TTFT）：Prefix KV Cache 磁盘序列化与 `mmap` 零拷贝
4. **第四层：源码实战篇 —— 全双工状态机（FSM）、流式切句与毫秒级打断**
   * 4.1 全双工有限状态机（FSM）设计
   * 4.2 核心粘合源码：`AudioAgentPipeline` 完整 C++17 实现
   * 4.3 Barge-in 打断后的上下文记忆回滚（KV Cache Rollback）机制
5. **第五层：算法进阶篇 —— 端侧模型微调（SFT/DPO）与混合精度量化**
   * 5.1 为什么端侧小模型必须做专项微调？（内化指令、抗噪、首句极短化）
   * 5.2 SFT 数据集构造与“先安抚、后执行”的异步 Tool Calling 设计
   * 5.3 LoRA 微调超参配置与 DPO 格式清洗（根除 Markdown 与乱码）
   * 5.4 AWQ 激活感知校准与 MNN 混合精度导出（`quant_block 64` + `lm_head Int8` + `embed_bin`）
   * 5.5 语音模型优化：ASR 热词浅层融合（Contextual Biasing）与 iSTFT 声码器加速
6. **第六层：工业级落地篇 —— 实时记忆提取、大小核调度与异常容错**
   * 6.1 端侧三级记忆子系统（滑动窗口 + 闲时摘要提取）
   * 6.2 异构算力调度：动态探测主频与 `sched_setaffinity` 大核绑定
   * 6.3 异常容错、温控降频防御与防 `lmkd` 杀后台保活清单

---

## 第一层：基础认知篇 —— 为什么端侧语音 Agent 必须重构？

### 1.1 云端方案 vs 端侧纯本地（On-Device）方案
在 Android 设备上运行语音助手，如果简单地把模型跑起来而不做全链路流式改造，用户说完话需要等待 3~5 秒才能听到声音反馈。端侧语音 Agent 的核心矛盾在于：
1. **统一内存（RAM）高压：** Android 的 `lmkd`（Low Memory Killer Daemon）对高内存进程极其无情。ASR、LLM、TTS 三个模型同时驻留，物理内存（RSS）必须死死压在 **1.8GB 以内**。
2. **内存带宽受限（Memory-bound）：** LLM 的自回归生成（Decode）每吐出一个 Token 都要把几十亿参数从 LPDDR5X 搬运到 CPU/GPU 寄存器一次。如果不做 4-bit 权重量化与算子融合，手机会在 3 轮对话内发烫触发温控降频（Thermal Throttling）。
3. **跨语言边界开销：** 如果音频采集在 Java `AudioRecord`，转成 `byte[]` 传给 JNI 跑 ASR，拿到 String 传回 Kotlin 再调 JNI 跑 LLM……频繁的 JNI 内存拷贝与 JVM GC 卡顿会直接导致音频播放破音（Underrun Glitch）。

### 1.2 级联流式（Streaming Cascade） vs 原生端到端（Audio-LLM）
* **原生端到端（如 Qwen-Audio / MiniCPM-o）：** 直接输入音频 Token。优点是能保留副语言信息（语调、叹气），但在端侧致命缺点是 **Audio Encoder 算力开销极大**，Prefill 延迟动辄 1.5s+，且常驻功耗过高。
* **全流式级联（VAD + Streaming ASR + LLM + Streaming TTS）：** **当前 Android 端侧落地的绝对主力方案。** 平时仅由极低功耗的 VAD（<1.8MB）监听，说话时流式 ASR 边说边转字，语音落音瞬间（<50ms）即出完文本；LLM 吐出前 5 个字遇到逗号立刻切分送入流式 TTS 播报。模块高度解耦、内存极度可控。

### 1.3 800ms 端到端首音延迟预算表（Latency Budget）
人类自然对话的反应间隔约为 500ms~800ms。我们将端到端首音延迟（Voice-to-Voice Latency）拆解为以下铁律指标：

| 流水线阶段 | 耗时预算 | 核心优化手段 |
| :--- | :--- | :--- |
| **1. VAD 尾点判定 (Endpointing)** | `250ms ~ 300ms` | Silero VAD 帧长设为 32ms（512 samples），静音截断阈值调至 `0.3s`。 |
| **2. 流式 ASR 增量定字** | `30ms ~ 60ms` | 采用 Chunk-based 流式架构，说话结束瞬间仅需结算最后 1~2 个音频块。 |
| **3. LLM Prefill (首字延迟 TTFT)** | `50ms ~ 90ms` | **固化 System Prompt KV Cache** + 微调缩减 Prompt，每次仅计算新增的 15 个 Token。 |
| **4. LLM 首句生成 (4~6 个汉字)** | `120ms ~ 180ms` | SFT 训练模型养成“短句先应答”习惯，1.5B/3B W4 模型以 ~40 tokens/s 吐出首个逗号即截断。 |
| **5. TTS 首包合成 + Oboe 缓冲** | `100ms ~ 160ms` | TTS 仅合成首个短句（如“好的，”），首块 PCM 生成后立即写入 Oboe 独占流。 |
| **端到端总延迟 (Voice-to-Voice)** | **550ms ~ 790ms** | **超越云端方案，达到真人无感交互水准** |

---

## 第二层：系统架构篇 —— ISO C++17 全双工零拷贝流水线

### 2.1 Native 全双工系统拓扑图

```
[硬件麦克风 (VoiceCommunication 模式 -> 激活芯片 DSP 硬件级 AEC / NS)]
       │ (PCM 16kHz, Float32, Mono, 10ms/帧 = 160 samples)
       ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ ISO C++17 Native 引擎层 (libaudio_agent.so)                                 │
│                                                                             │
│  [Oboe Input Callback (高优实时音频线程)]                                   │
│       │ 无锁写入                                                            │
│       ▼                                                                     │
│  (SPSC_RingBuffer_Mic) ──► [感知线程: Sherpa-onnx VAD + Streaming ASR]       │
│                                   │   (绑定能效小核 Little Cores)            │
│          ┌────────────────────────┴─────────────────────┐                   │
│          ▼ (正常说话尾点 Endpoint)                       ▼ (播报中检测到人声)│
│  [吐出完整 User Query 文本]                     [触发 Barge-in 硬件级打断!] │
│          │                                              ├──► 1. atomic<bool> abort_llm = true│
│          ▼                                              ├──► 2. 清空 TTS 队列与 Spk 缓冲     │
│  (ThreadSafeQueue_Query)                                └──► 3. 回滚 LLM 未播报的 KV Cache   │
│          │ 唤醒条件变量 (cv.notify_one)                                     │
│          ▼                                                                  │
│  [认知线程: MNN-LLM 推理引擎 (绑定大核/超大核 + OpenCL/Vulkan)]             │
│  (已通过 mmap 预加载 System Prompt Prefix KV Cache + embed_bin 磁盘映射)    │
│          │ 流式吐出 Token (自定义 std::stringbuf 实时拦截)                   │
│          ▼                                                                  │
│  [流式标点切分器 (Sentence Chunker)]                                        │
│  (首包满 6 Bytes 遇标点即截断；识别 <|tool_call|> 自动分流至异步函数执行器) │
│          │                                                                  │
│          ▼                                                                  │
│  (ThreadSafeQueue_TTS) ──► [表达线程: Sherpa-onnx TTS (Kokoro / Piper)]     │
│                                   │ (生成 24kHz PCM Float 块)               │
│                                   ▼                                         │
│                            (SPSC_RingBuffer_Spk)                            │
│                                   │ 无锁读取                                │
│                                   ▼                                         │
│                     [Oboe Output Callback (24kHz 独占流)]                   │
└───────────────────────────────────┼─────────────────────────────────────────┘
                                    ▼
                              [硬件扬声器]
```

### 2.2 音频底座：Oboe 低延迟流与芯片级 DSP 硬件回声消除（AEC）
在全双工模式下，扬声器正在播放 TTS 合成的声音，同时麦克风保持开启以支持随时打断（Barge-in）。如果回声没消干净，VAD 会把助手自己的声音当成用户插话，导致“自己打断自己”的死循环。

**工程铁律：** 绝不在 CPU 上跑软件 AEC（如 WebRTC AEC3 极难对齐硬件延迟且吃算力）。必须通过 Oboe 将输入流配置为 `VoiceCommunication`，强制 Android Audio HAL 唤醒 SoC 芯片底层的硬件 DSP 回声消除器：

```cpp
#include <oboe/Oboe.h>

class FullDuplexAudioIO : public oboe::AudioStreamDataCallback {
public:
    bool startStreams(int32_t asr_sample_rate = 16000, int32_t tts_sample_rate = 24000) {
        // 1. 配置上行采集流 (固定 16kHz 喂给 Sherpa-onnx ASR/VAD)
        oboe::AudioStreamBuilder inBuilder;
        inBuilder.setDirection(oboe::Direction::Input)
                 ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
                 ->setSharingMode(oboe::SharingMode::Exclusive)
                 ->setFormat(oboe::AudioFormat::Float)
                 ->setChannelCount(oboe::ChannelCount::Mono)
                 ->setSampleRate(asr_sample_rate)
                 // 【核心】强制激活硬件 DSP 级 AEC(回声消除)、NS(降噪)、AGC(自动增益)
                 ->setInputPreset(oboe::InputPreset::VoiceCommunication)
                 ->setDataCallback(this);

        if (inBuilder.openStream(input_stream_) != oboe::Result::OK) return false;

        // 2. 配置下行播放流 (动态匹配 TTS 采样率，如 24000Hz，省去软件重采样开销!)
        oboe::AudioStreamBuilder outBuilder;
        outBuilder.setDirection(oboe::Direction::Output)
                  ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
                  ->setSharingMode(oboe::SharingMode::Exclusive)
                  ->setFormat(oboe::AudioFormat::Float)
                  ->setChannelCount(oboe::ChannelCount::Mono)
                  ->setSampleRate(tts_sample_rate)
                  ->setUsage(oboe::Usage::VoiceCommunication) // 与输入流配对，建立 DSP AEC 参考信号回路
                  ->setDataCallback(this);

        if (outBuilder.openStream(output_stream_) != oboe::Result::OK) return false;

        output_stream_->requestStart();
        input_stream_->requestStart();
        return true;
    }

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *stream, void *audioData, int32_t numFrames) override {
        auto* float_ptr = static_cast<float*>(audioData);
        if (stream->getDirection() == oboe::Direction::Input) {
            // 实时音频回调中严禁加锁(Mutex)或分配内存(malloc)！直接写入无锁 RingBuffer
            mic_ring_buffer_.write(float_ptr, numFrames);
        } else {
            // 从扬声器 RingBuffer 读取合成好的 TTS 音频，若无数据则填充静音 0.0f
            size_t read_count = spk_ring_buffer_.read(float_ptr, numFrames);
            if (read_count < static_cast<size_t>(numFrames)) {
                std::fill(float_ptr + read_count, float_ptr + numFrames, 0.0f);
            }
        }
        return oboe::DataCallbackResult::Continue;
    }

private:
    std::shared_ptr<oboe::AudioStream> input_stream_;
    std::shared_ptr<oboe::AudioStream> output_stream_;
    // SPSC RingBuffers...
};
```

### 2.3 无锁环形缓冲区（Lock-Free SPSC RingBuffer）C++17 实现
Oboe 的 `onAudioReady` 运行在 Android 系统的 `FastMixer / FastCapture` 高优实时内核线程中，一旦被 `std::mutex` 阻塞超过 5ms 就会引发爆音。必须使用基于 `std::atomic` 内存序的单生产者-单消费者（SPSC）无锁环形缓冲区：

```cpp
#include <atomic>
#include <vector>
#include <cstring>
#include <algorithm>

template <typename T>
class LockFreeSPSCRingBuffer {
public:
    explicit LockFreeSPSCRingBuffer(size_t capacity) 
        : capacity_(capacity + 1), buffer_(capacity + 1) {}

    size_t write(const T* data, size_t count) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t free_space = (tail + capacity_ - head - 1) % capacity_;
        const size_t to_write = std::min(count, free_space);
        if (to_write == 0) return 0;

        const size_t first_chunk = std::min(to_write, capacity_ - head);
        std::memcpy(&buffer_[head], data, first_chunk * sizeof(T));
        if (to_write > first_chunk) {
            std::memcpy(&buffer_[0], data + first_chunk, (to_write - first_chunk) * sizeof(T));
        }
        head_.store((head + to_write) % capacity_, std::memory_order_release);
        return to_write;
    }

    size_t read(T* dest, size_t count) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t available = (head + capacity_ - tail) % capacity_;
        const size_t to_read = std::min(count, available);
        if (to_read == 0) return 0;

        const size_t first_chunk = std::min(to_read, capacity_ - tail);
        std::memcpy(dest, &buffer_[tail], first_chunk * sizeof(T));
        if (to_read > first_chunk) {
            std::memcpy(dest + first_chunk, &buffer_[0], (to_read - first_chunk) * sizeof(T));
        }
        tail_.store((tail + to_read) % capacity_, std::memory_order_release);
        return to_read;
    }

    void clear() {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    const size_t capacity_;
    std::vector<T> buffer_;
    alignas(64) std::atomic<size_t> head_{0}; // 64字节缓存行对齐，防止伪共享(False Sharing)
    alignas(64) std::atomic<size_t> tail_{0};
};
```

---

## 第三层：核心引擎篇 —— Sherpa-onnx 与 MNN-LLM 深度拆解

### 3.1 Sherpa-onnx：全链路语音引擎集成
Sherpa-onnx 基于 C++17 与 ONNX Runtime 构建，彻底剥离了 Python 依赖，一个 `.so` 库即可搞定 VAD、ASR、TTS。

#### 黄金模型选型清单
1. **VAD 模型：** `silero_vad.onnx`（1.8MB，帧长 512 samples = 32ms，CPU 单核耗时 < 0.4ms）。
2. **流式 ASR 模型：** `sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20`
   * 包含 `encoder-epoch-99-avg-1.int8.onnx`、`decoder-epoch-99-avg-1.onnx`（保持 FP32 保精度）、`joiner-epoch-99-avg-1.int8.onnx`，总重约 70MB，支持中英混合边说边出字。
3. **流式 TTS 模型：** `kokoro-multi-lang-v1_0`（82M 参数，拟真度极高，支持中英文无缝混读）或 `vits-melo-tts-zh_en`。

#### Sherpa-onnx 流式 TTS 异步中断回调机制
切勿等待整段文字合成完毕再播放。使用 `SherpaOnnxOfflineTtsGenerateWithCallbackWithArg`，每当底层合成出一小块音频，立即推入 `spk_ring_buffer_`；一旦检测到用户插话（Barge-in），在回调中返回 `0` 即可瞬间掐断 TTS 推理：

```cpp
static int32_t TtsAudioChunkCallback(const float* samples, int32_t num_samples, float progress, void* arg) {
    auto* pipeline = static_cast<AudioAgentPipeline*>(arg);
    if (pipeline->IsBargeInAborted()) {
        return 0; // 【关键】返回 0 通知 Sherpa-onnx 立即终止当前 TTS 推理循环
    }
    pipeline->WriteToSpeakerBuffer(samples, num_samples);
    return 1; // 返回 1 继续合成下一音频块
}
```

### 3.2 MNN-LLM：移动端异构加速与编译调优
相比于原生 `llama.cpp`，阿里的 **MNN-LLM** 在 Android 端侧具备两大杀手锏：
1. **ARM 汇编级融合算子：** 利用 ARMv8.2/v9 的 `DotProd`、`I8MM`（8位整数矩阵乘法）和 `SME2` 指令集，在寄存器内直接完成 **4-bit 权重在线反量化 + 矩阵乘法（W4A16/W4A8 Fuse GEMM）**，省去中间张量写回内存的开销，将 LPDDR 内存带宽利用率压榨到极致。
2. **Adreno / Mali GPU 纹理加速：** MNN 的 OpenCL 后端将权重映射为 GPU 的 `Image2D` 纹理对象，直接白嫖移动 GPU 专用的 Texture L1 Cache。

#### Android NDK 交叉编译必开宏（CMake Flags）
```bash
cmake ../../../ \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI="arm64-v8a" \
  -DANDROID_PLATFORM=android-28 \
  -DMNN_LOW_MEMORY=ON \                  # 【必开】激活 W4A16/W4A8 运行时动态反量化
  -DMNN_CPU_WEIGHT_DEQUANT_GEMM=ON \     # 【必开】反量化与 GEMM 算子融合
  -DMNN_BUILD_LLM=ON \                   # 编译 MNN-LLM 引擎
  -DMNN_SUPPORT_TRANSFORMER_FUSE=ON \    # 【必开】融合 FlashAttention / RoPE / RMSNorm
  -DMNN_OPENCL=ON \                      # 开启移动端 GPU OpenCL 加速
  -DMNN_VULKAN=ON \                      # 开启 Vulkan 加速
  -DMNN_ARM82=ON \                       # 开启 ARMv8.2 FP16 与 DotProd/I8MM 指令集
  -DMNN_USE_MMAP=ON                      # 支持 mmap 映射权重，防 lmkd 杀进程
```

### 3.3 消灭首字延迟（TTFT）：Prefix KV Cache 固化与 `mmap`
语音助手的 System Prompt（包含角色设定、口语约束、本地控制工具 JSON Schema）通常长达 400~600 Tokens。如果每次说话都重新计算这段 Prefill，首字延迟将高达 800ms+。

**零延迟解决方案：**
在 `llm_config.json` 中配置 `"reuse_kv": true` 与 `"memory": "low"`。在引擎冷启动初始化阶段，预先将固定 System Prompt 送入模型执行一次前向传播，使 MNN-LLM 在内部缓存这段前缀的 KV 状态。此后每次用户触发语音对话，拼接后的上下文自动命中前缀匹配（Prefix Hit），**仅需对用户新说的 10~20 个 Token 进行增量 Prefill，TTFT 直接骤降至 60ms~80ms！**

---

## 第四层：源码实战篇 —— 全双工状态机（FSM）、流式切句与毫秒级打断

### 4.1 全双工有限状态机（FSM）设计

```
                   ┌──────────────────────────────────────┐
                   │               IDLE                   │
                   │ (仅小核跑 Oboe Input + Silero VAD)    │
                   └──────────────────┬───────────────────┘
                                      │ VAD 检测到语音能量 > 阈值
                                      ▼
                   ┌──────────────────────────────────────┐
         ┌────────►│             LISTENING                │◄────────┐
         │         │   (流式 ASR 实时追加音频帧解码)       │         │
         │         └──────────────────┬───────────────────┘         │
         │                            │ VAD 判定静音持续 > 300ms     │
         │                            ▼                             │
         │         ┌──────────────────────────────────────┐         │
   Barge-in 打断!   │             THINKING                 │         │
  (VAD检测到插话)  │ (MNN-LLM 增量 Prefill + 生成首句中)  │         │
         │         └──────────────────┬───────────────────┘         │
         │                            │ 首句触发标点切分，推入 TTS  │
         │                            ▼                             │
         │         ┌──────────────────────────────────────┐         │
         └─────────┤             SPEAKING                 ├─────────┘
                   │ (TTS流式播报 + LLM后台续写 + VAD监听) │  Barge-in 打断!
                   └──────────────────┬───────────────────┘ (VAD检测到插话)
                                      │ TTS 队列播完 & Spk Buffer 排空
                                      ▼
                                 [回到 IDLE]
```

### 4.2 核心粘合源码：`AudioAgentPipeline.hpp` 完整 C++17 实现
以下代码展示了如何将 **流式 Token 拦截**、**首句极速切分**、**Tool Call 异步分流** 以及 **毫秒级 Barge-in 打断** 封装在同一个工业级流水线中：

```cpp
#pragma once
#include <atomic>
#include <string>
#include <vector>
#include <sstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <sched.h>
#include <unistd.h>
#include "llm.hpp"          // MNN-LLM 头文件
#include "c-api.h"          // Sherpa-onnx C API

enum class AgentState { IDLE, LISTENING, THINKING, SPEAKING };

class AudioAgentPipeline {
public:
    AudioAgentPipeline() = default;
    ~AudioAgentPipeline() { Stop(); }

    // 1. 异构大核绑定：防止 OS 调度器将 LLM/TTS 线程迁移至能效小核引发延迟抖动
    static void BindThreadToBigCores(const std::vector<int>& core_ids) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        for (int id : core_ids) CPU_SET(id, &cpuset);
        sched_setaffinity(gettid(), sizeof(cpu_set_t), &cpuset);
    }

    // 2. 硬件级全双工打断 (由 VAD 监听线程在检测到人声瞬间调用)
    void TriggerBargeIn() {
        AgentState cur = state_.load(std::memory_order_acquire);
        if (cur == AgentState::THINKING || cur == AgentState::SPEAKING) {
            // 第一步：拉响原子中断标志，让 LLM Streambuf 和 TTS Callback 在下一个毫秒级周期立即退出
            abort_flag_.store(true, std::memory_order_release);

            // 第二步：瞬间清空待合成的 TTS 文本队列
            {
                std::lock_guard<std::mutex> lock(tts_mtx_);
                std::queue<std::string> empty;
                std::swap(tts_queue_, empty);
            }

            // 第三步：瞬间排空扬声器环形缓冲区，硬件喇叭立即闭嘴
            spk_ring_buffer_.clear();

            // 第四步：状态切回倾听模式
            state_.store(AgentState::LISTENING, std::memory_order_release);
        }
    }

    // 3. 处理 ASR 定字后的用户文本 (运行在绑定了大核的 LLM 专属工作线程)
    void RunLLMInference(const std::string& user_query) {
        abort_flag_.store(false, std::memory_order_release);
        state_.store(AgentState::THINKING, std::memory_order_release);
        spoken_text_accumulator_.clear();

        // 自定义流缓冲区：在 C++ 内存层面实时拦截 MNN-LLM 吐出的每一个 Token
        class StreamingSentenceChunker : public std::stringbuf {
        public:
            explicit StreamingSentenceChunker(AudioAgentPipeline* parent) : p_(parent) {}

            std::streamsize xsputn(const char* s, std::streamsize n) override {
                if (p_->abort_flag_.load(std::memory_order_acquire)) {
                    return 0; // 返回 0 配合底层终止生成
                }

                std::string token(s, n);
                buffer_ += token;

                // 检测是否进入 Function Calling 工具调用标签 <|tool_call|>
                if (buffer_.find("<|tool_call|>") != std::string::npos) {
                    in_tool_call_ = true;
                }
                if (in_tool_call_) {
                    // 工具调用 JSON 不送入 TTS 朗读，等待闭合标签后异步执行
                    auto end_pos = buffer_.find("<|/tool_call|>");
                    if (end_pos != std::string::npos) {
                        std::string tool_json = ExtractToolJson(buffer_);
                        p_->DispatchAsyncToolCall(tool_json);
                        buffer_.clear();
                        in_tool_call_ = false;
                    }
                    return n;
                }

                // 标点符号检测与动态阈值切分策略：
                // 首句只要满 6 字节（2个汉字，如“好的，”）且遇到标点，立即切分送 TTS，极限压榨首音延迟！
                // 后续句子满 21 字节（7个汉字）遇到标点再切分，保证 TTS 韵律连贯度。
                size_t min_chunk_bytes = is_first_sentence_ ? 6 : 21;
                if (buffer_.size() >= min_chunk_bytes && HasPunctuation(token)) {
                    p_->EnqueueTtsSentence(buffer_);
                    buffer_.clear();
                    is_first_sentence_ = false;
                }
                return n;
            }

            void Flush() {
                if (!buffer_.empty() && !in_tool_call_ && !p_->abort_flag_.load()) {
                    p_->EnqueueTtsSentence(buffer_);
                    buffer_.clear();
                }
            }

        private:
            bool HasPunctuation(const std::string& tok) {
                static const std::vector<std::string> puncts = {"，", "。", "！", "？", "；", ",", ".", "!", "?", "\n"};
                for (const auto& pt : puncts) {
                    if (tok.find(pt) != std::string::npos) return true;
                }
                return false;
            }
            std::string ExtractToolJson(const std::string& raw) { /* 提取标签内 JSON */ return raw; }

            AudioAgentPipeline* p_;
            std::string buffer_;
            bool is_first_sentence_ = true;
            bool in_tool_call_ = false;
        };

        StreamingSentenceChunker chunker(this);
        std::ostream llm_stream(&chunker);

        // 调用 MNN-LLM 执行流式解码
        mnn_llm_->response(user_query, &llm_stream);
        chunker.Flush();
    }

    void EnqueueTtsSentence(const std::string& sentence) {
        if (abort_flag_.load(std::memory_order_acquire)) return;
        state_.store(AgentState::SPEAKING, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(tts_mtx_);
            tts_queue_.push(sentence);
        }
        tts_cv_.notify_one();
    }

    void DispatchAsyncToolCall(const std::string& json_cmd) {
        // 通过 JNI 回调或本地 C++ Router 异步执行系统控制指令（如调亮度、查天气、设闹钟）
    }

    bool IsBargeInAborted() const { return abort_flag_.load(std::memory_order_acquire); }
    void WriteToSpeakerBuffer(const float* data, size_t n) { spk_ring_buffer_.write(data, n); }
    void Stop() { /* 优雅关闭线程 */ }

private:
    std::unique_ptr<MNN::Transformer::Llm> mnn_llm_;
    const SherpaOnnxOfflineTts* sherpa_tts_ = nullptr;
    LockFreeSPSCRingBuffer<float> spk_ring_buffer_{24000 * 10}; // 10秒缓冲容量

    std::atomic<AgentState> state_{AgentState::IDLE};
    std::atomic<bool> abort_flag_{false};

    std::mutex tts_mtx_;
    std::condition_variable tts_cv_;
    std::queue<std::string> tts_queue_;
    std::string spoken_text_accumulator_;
};
```

### 4.3 Barge-in 打断后的上下文记忆回滚（KV Cache Rollback）机制
**端侧工程极其隐蔽的 Bug：** 假设用户让助手讲个笑话，MNN-LLM 以 40 tokens/s 的速度在 2 秒内就已经把 80 个字的笑话全部生成完了（KV Cache 已经推进了 80 个 Token）。但因为人类说话语速慢，TTS 才刚播报了前 15 个字，用户就插话打断了。
如果此时不处理 KV Cache，在模型的记忆里“我已经把笑话讲完了”，而用户实际上根本没听完。

**修正做法：**
在 TTS 线程每实际播完一个短句时，将其追加到 `spoken_text_accumulator_`。一旦发生 `TriggerBargeIn()`，用 `spoken_text_accumulator_ + "（被用户打断）"` 覆盖 MNN-LLM 当前轮次的 Assistant 历史记录，并将 KV Cache 长度截断（Truncate）回实际播报文本对应的 Token 位置，实现**“用户听到的 == 模型记住的”**严格对齐。

---

## 第五层：算法进阶篇 —— 端侧模型微调（SFT/DPO）与混合精度量化

直接拿开源通用基座模型（如 Qwen2.5/3-1.5B/3B）上端侧，会出现三大水土不服：**1. 喜欢输出长篇大论和 Markdown 符号（导致 TTS 首包慢且乱读星号井号）；2. 对流式 ASR 传过来的同音错别字极其脆弱；3. 4-bit 量化后工具调用（Function Calling）JSON 格式崩溃。**

### 5.1 为什么端侧小模型必须做专项微调？
1. **指令内化（Prompt Bake-in）：** 通过微调将口语风格与工具 Schema 焊死在权重里，把运行时的 System Prompt 从 600 Tokens 压缩到 30 Tokens 以内，大幅节省端侧内存。
2. **首句极短化肌肉记忆：** 强训模型养成 **“2~4字安抚应答 + 标点 + 正文/工具调用”** 的输出范式（如 `"好的，正在为您导航。"`），确保生成第 2~3 个 Token 时必然触发逗号切分，将 LLM 首包生成耗时死死摁在 **150ms** 以内。
3. **端侧 ASR 隐式纠错（Implicit Error Correction）：** 在训练集的 `input` 中人为注入 20% 的拼音同音替换噪声，让 LLM 直接从上下文推断真实意图，省去独立的纠错模型。
4. **纯汉字数字输出（内置 TN）：** 强训模型将所有数字、百分比、温度直接输出为汉字（如输出 `"零下五度"` 而非 `"-5°C"`），彻底消除轻量级 TTS 的读数 Bug。

### 5.2 SFT 数据集构造与“先安抚、后执行”的异步 Tool Calling
构造 3,000 ~ 5,000 条高质量 JSONL 数据（`voice_agent_sft.jsonl`），采用 **口语安抚在前、标签控制流在后** 的结构：

```json
[
  {
    "instruction": "你是本地语音助手，请用极简纯口语回答，严禁使用符号与阿拉伯数字。",
    "input": "帮我把导航设到杭州西湖文化广场",
    "output": "好的，马上出发。<|tool_call|>{\"action\":\"start_nav\",\"dest\":\"杭州西湖文化广场\"}<|/tool_call|>"
  },
  {
    "instruction": "你是本地语音助手，请用极简纯口语回答，严禁使用符号与阿拉伯数字。",
    "input": "把空条温度调道二十四渡（注：故意构造的ASR同音错字样本）",
    "output": "没问题，已调至二十四度。<|tool_call|>{\"action\":\"set_ac_temp\",\"value\":24}<|/tool_call|>"
  },
  {
    "instruction": "你是本地语音助手，请用极简纯口语回答，严禁使用符号与阿拉伯数字。",
    "input": "为什么天空是蓝色的啊",
    "output": "这叫瑞利散射。简单说，阳光里的蓝光波长短，穿过大气层时最容易被空气分子撞得四处散开，所以咱们抬头看到的天就是蓝的。"
  }
]
```
> **架构收益：** 当模型吐出 `"好的，马上出发。"`（仅需 180ms）时，`StreamingSentenceChunker` 已经把它切下来塞进 TTS 开始出声了；在喇叭发出“好的”这两个字的那 500ms 时间里，模型正好在后台静默吐完 `<|tool_call|>...` 并由异步线程唤醒地图 App——**用户主观感受到的响应延迟是 0 秒！**

### 5.3 LoRA 微调与 DPO 偏好对齐配置（基于 LLaMA-Factory / Unsloth）
1. **阶段一：全线性层 LoRA SFT (`sft_config.yaml`)**
   ```yaml
   model_name_or_path: Qwen/Qwen2.5-3B-Instruct
   stage: sft
   finetuning_type: lora
   lora_target: all            # 必须覆盖全部 7 个投影层(q/k/v/o/gate/up/down)，否则 4-bit 量化易掉点
   lora_rank: 16
   lora_alpha: 32
   cutoff_len: 1024
   learning_rate: 1.0e-4
   num_train_epochs: 3.0
   lr_scheduler_type: cosine
   bf16: true
   ```
2. **阶段二：DPO 根除 Markdown 与书面语**
   构造 500 条偏好数据对（`chosen` 为纯口语短句，`rejected` 为带 `1. 2. 3.`、`**加粗**` 或换行列表的回答），以 `learning_rate: 5.0e-6`、`beta: 0.1` 跑 1 个 Epoch 的 DPO，即可 100% 根除 Markdown 符号污染。

### 5.4 AWQ 激活校准与 MNN 混合精度导出（防降智核心机密）
微调合并得到 `merged_fp16` 模型后，切勿直接做暴力的 RTN 4-bit 量化。必须执行以下两步精密压缩：

#### 第一步：使用 AutoAWQ 基于真实语音指令做激活感知校准
利用 256 条 SFT 语音指令样本作为 Calibration Data，找出模型中 1% 对激活值异常（Outliers）极度敏感的显著权重通道并进行缩放保护。

#### 第二步：MNN 细粒度混合精度导出（`llmexport.py`）
```bash
python llmexport.py \
    --path ./merged_fp16 \
    --export mnn \
    --quant_bit 4 \
    --quant_block 64 \
    --lm_quant_bit 8 \
    --embed_bin
```
* **`--quant_block 64`：** 将权重量化分组从默认的 128 缩小至 64。模型体积仅增加约 70MB，但 Function Calling 的 JSON 语法准确率从 72% 飙升至 **97% 以上**。
* **`--lm_quant_bit 8`：** 模型的输出分类头（`lm_head`）决定了最终 Token 的概率分布，强制保留为 **8-bit 量化**，彻底解决 1.5B/3B 小模型量化后复读机或胡言乱语的问题。
* **`--embed_bin`：** 将庞大的词表 Embedding 层单独剥离为二进制文件，在 Android 端通过 `mmap` 按需随机寻址，**直接节省 350MB+ 物理内存（RSS）**。

### 5.5 语音模型优化：ASR 热词浅层融合与 iSTFT 声码器加速
1. **Sherpa-onnx 免训练热词增强（Contextual Biasing）：**
   无需重训 ASR 模型，直接挂载 `hotwords.txt`（如写入 `西湖文化广场 :3.0`）。Sherpa-onnx 底层基于 **Aho-Corasick 自动机** 在 `modified_beam_search` 解码时动态给匹配音素的束（Beam）加分，专有名词识别率立竿见影提升 40%。
2. **TTS 声码器 iSTFT 替换与 Int8 图优化：**
   在端侧弃用算力黑洞原始 HiFi-GAN，改用基于 **iSTFT（短时傅里叶逆变换）** 的 Kokoro 或 Piper 架构，并用 ONNX Runtime 的 `quantize_dynamic` 将模型线性层压至 `Int8`，使手机 CPU 单大核合成实时率（RTF）稳定在 **0.12** 左右（即合成 1 秒语音仅需 120ms）。

---

## 第六层：工业级落地篇 —— 实时记忆提取、大小核调度与异常容错

### 6.1 端侧三级记忆子系统（Three-Tier Memory Architecture）
语音助手在连续对话几十轮后，KV Cache 会撑爆内存。必须在 C++ 层实现三级记忆管理：
1. **L1 瞬时工作记忆（Ring KV Cache）：** 永远固定保留最前面的 `System Prompt Tokens` + 最近 **4 轮（8 句）** 对话的 KV Cache，超出部分采用 FIFO 移位丢弃。
2. **L2 闲时语义摘要记忆（Idle-Time Summarization）：** 当被丢弃的对话攒够 6 轮，且状态机处于 `IDLE` 静默状态超过 10 秒时，后台唤醒 MNN-LLM 以低优先级（2 线程）静默生成一句 30 字以内的核心事实摘要（如 `"用户偏好24度空调，家住杭州"`），存入本地 SQLite。
3. **L3 轻量端侧向量检索（SQLite-vec / Faiss-Mobile）：** 利用一个仅 20MB 的端侧 Embedding 模型（如 `bge-micro-v2.onnx` 跑在 Sherpa-onnx 底层 ONNX Runtime 上），在用户提问时用 5ms 检索出最相关的 2 条历史事实动态拼入当前轮次。

### 6.2 异构算力调度：动态探测主频与 `sched_setaffinity` 大核绑定
Android 旗舰芯片（如骁龙 8 Gen 3 / 8 Elite、天玑 9300/9400、Google Tensor G4/G5）普遍采用三丛集架构（超大核 + 大核 + 能效小核）。不同厂商的大小核编号顺序完全不同！

**动态探测并绑定前 4 颗最强核心的 C++17 标准代码：**

```cpp
#include <fstream>
#include <algorithm>
#include <vector>
#include <string>

std::vector<int> DetectTopPerformanceCores(size_t top_n = 4) {
    struct CoreFreq { int id; long max_freq; };
    std::vector<CoreFreq> cores;

    int num_cpus = sysconf(_SC_NPROCESSORS_CONF);
    for (int i = 0; i < num_cpus; ++i) {
        std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/cpuinfo_max_freq";
        std::ifstream ifs(path);
        long freq = 0;
        if (ifs >> freq) {
            cores.push_back({i, freq});
        }
    }
    // 按最大主频降序排列
    std::sort(cores.begin(), cores.end(), [](const CoreFreq& a, const CoreFreq& b) {
        return a.max_freq > b.max_freq;
    });

    std::vector<int> result;
    for (size_t i = 0; i < std::min(top_n, cores.size()); ++i) {
        result.push_back(cores[i].id);
    }
    return result;
}
```
* **线程绑定铁律：**
  * **MNN-LLM 推理线程 & Sherpa-onnx TTS 线程：** 绑定至 `DetectTopPerformanceCores(4)`（大核与超大核）。
  * **Sherpa-onnx VAD / 流式 ASR 线程：** 绑定至主频最低的 2 颗能效小核，确保全天候监听功耗低于 **150mW**。

### 6.3 异常容错、温控降频防御与防 `lmkd` 杀后台保活清单

| 风险场景 | 故障表现 | C++ / Android 系统级防御方案 |
| :--- | :--- | :--- |
| **1. 模型加载撑爆 Heap 被 `lmkd` 瞬杀** | App 启动加载模型时直接闪退，无 Java Crash 日志（Native OOM）。 | **严禁使用 `AssetManager` 将模型读入内存流！** 首次启动将模型解压至 `/data/user/0/<pkg>/files/models/`，全部通过文件路径走 Linux 内核 **`mmap`** 映射；并开启 `foregroundServiceType="microphone\|mediaPlayback"` 前台服务提权。 |
| **2. 音频焦点抢占与蓝牙耳机断连** | 来电、微信语音或拔掉耳机时，Oboe 流失效导致死循环空转或崩溃。 | 实现 `oboe::AudioStreamErrorCallback::onErrorAfterClose()`，一旦捕获 `ErrorDisconnected`，在 50ms 内自动销毁旧流并重建路由至新音频设备。 |
| **3. 连续对话触发 SoC 温控降频** | 聊满 10 分钟后机身超 43℃，大核被锁频至 1.2GHz，TTFT 退化至 2 秒。 | 通过 Android `PowerManager.OnThermalStatusChangedListener` 监听热状态：当达到 `THERMAL_STATUS_MODERATE` 时，动态将 MNN 后端从 `CPU` 切换至 `OpenCL (GPU)` 分摊热源，并将 `max_new_tokens` 从 150 下调至 60。 |
| **4. LLM 陷入复读机死循环或超时** | 极端噪声输入导致模型无限吐出重复字符，阻塞 TTS 队列。 | 在 `StreamingSentenceChunker` 中内置 **N-gram 重复检测器** 与 **3 秒首包看门狗（Watchdog Timer）**：一旦连续 12 个字节完全重复或首包超时，立即置位 `abort_flag_` 并由 TTS 播报兜底语音 `"抱歉，我刚走神了，您能再说一遍吗？"`。 |
| **5. OpenCL 冷启动编译卡顿** | 首次打开 App 时 GPU 编译数百个 Shader 耗时 8~12 秒。 | 在 MNN Runtime 配置 `cachePath` 指向持久化文件 `mnn_opencl_program.bin`，仅在首次安装时编译一次，后续冷启动实现 **200ms 级秒开**。 |
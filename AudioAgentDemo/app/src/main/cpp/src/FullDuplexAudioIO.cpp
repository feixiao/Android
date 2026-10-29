#include "FullDuplexAudioIO.hpp"
#include <algorithm>
#include <thread>
#include <chrono>

namespace audio_agent {

/**
 * @brief 构造函数：注入麦克风与扬声器的无锁单生产者单消费者（SPSC）环形缓冲区
 */
FullDuplexAudioIO::FullDuplexAudioIO(
    std::shared_ptr<LockFreeSPSCRingBuffer<float>> mic_ring_buffer,
    std::shared_ptr<LockFreeSPSCRingBuffer<float>> spk_ring_buffer,
    int32_t mic_sample_rate,
    int32_t spk_sample_rate
) : mic_ring_buffer_(std::move(mic_ring_buffer)),
    spk_ring_buffer_(std::move(spk_ring_buffer)),
    mic_sample_rate_(mic_sample_rate),
    spk_sample_rate_(spk_sample_rate) {}

FullDuplexAudioIO::~FullDuplexAudioIO() {
    StopStreams();
}

/**
 * @brief 启动底层 Oboe 全双工音频流（录音流 + 放音流并行启动）
 * 
 * 核心配置考量：
 * 1. 录音流（Input Stream）：
 *    - SampleRate: 16kHz（语音识别 ASR 与 VAD 的黄金标准采样率，免去实时重采样损耗）；
 *    - InputPreset: VoiceCommunication（硬核激活高通/联发科 DSP 芯片级硬件 AEC 回声消除、降噪 NS 和自动增益 AGC）；
 *    - PerformanceMode: LowLatency（请求 AAudio 硬件快速路径，绕过系统软混音器）。
 * 2. 播音流（Output Stream）：
 *    - SampleRate: 24kHz（现代端侧 TTS 常见高质量采样率）；
 *    - Usage: VoiceCommunication（必须与 InputPreset 配合使用，让音频驱动将播音音频精准回路输入给硬件 AEC 模块作为参考信号 Ref-signal）；
 *    - SharingMode: Exclusive（独占模式，直通音频硬件降低抖动）。
 */
bool FullDuplexAudioIO::StartStreams() {
    if (is_running_.load(std::memory_order_acquire)) {
        return true;
    }

#if defined(AUDIO_AGENT_USE_OBOE)
    // 1. 配置录音流（16kHz Mono，语音通信专属预设）
    oboe::AudioStreamBuilder inBuilder;
    inBuilder.setDirection(oboe::Direction::Input)
             ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
             ->setSharingMode(oboe::SharingMode::Shared)
             ->setFormat(oboe::AudioFormat::Float)
             ->setChannelCount(oboe::ChannelCount::Mono)
             ->setSampleRate(mic_sample_rate_)
             ->setInputPreset(oboe::InputPreset::VoiceCommunication) // 硬件 AEC/NS/AGC
             ->setDataCallback(this)
             ->setErrorCallback(this);

    oboe::Result res = inBuilder.openStream(input_stream_);
    if (res != oboe::Result::OK) {
        return false;
    }

    // 2. 配置播放流（24kHz，VoiceCommunication 用途用于回声参考回路）
    oboe::AudioStreamBuilder outBuilder;
    outBuilder.setDirection(oboe::Direction::Output)
              ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
              ->setSharingMode(oboe::SharingMode::Exclusive)
              ->setFormat(oboe::AudioFormat::Float)
              ->setChannelCount(oboe::ChannelCount::Mono)
              ->setSampleRate(spk_sample_rate_)
              ->setUsage(oboe::Usage::VoiceCommunication) // 与录音流配对构成 DSP AEC 闭环
              ->setDataCallback(this)
              ->setErrorCallback(this);

    res = outBuilder.openStream(output_stream_);
    if (res != oboe::Result::OK) {
        input_stream_->close();
        input_stream_.reset();
        return false;
    }

    // 并行请求启动硬件流
    output_stream_->requestStart();
    input_stream_->requestStart();
#endif

    is_running_.store(true, std::memory_order_release);
    return true;
}

/**
 * @brief 停止并释放音频流
 */
void FullDuplexAudioIO::StopStreams() {
    if (!is_running_.load(std::memory_order_acquire)) return;

    is_running_.store(false, std::memory_order_release);

#if defined(AUDIO_AGENT_USE_OBOE)
    if (input_stream_) {
        input_stream_->stop();
        input_stream_->close();
        input_stream_.reset();
    }
    if (output_stream_) {
        output_stream_->stop();
        output_stream_->close();
        output_stream_.reset();
    }
#endif
}

/**
 * @brief 路由热重连（如拔插耳机、蓝牙断开触发）
 * 
 * 采用 CAS 原子标志位防止并发重复重启，睡眠 20ms 等待 Android 音频 HAL 层路由完成拓扑切换。
 */
bool FullDuplexAudioIO::RestartStreams() {
    bool expected = false;
    if (!is_restarting_.compare_exchange_strong(expected, true)) {
        return false; // 正在重启中，直接忽略并发请求
    }

    StopStreams();
    // 等待硬件音频 HAL 拓扑完成稳定（通常在 20~50ms 内）
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    bool success = StartStreams();

    is_restarting_.store(false, std::memory_order_release);
    return success;
}

#if defined(AUDIO_AGENT_USE_OBOE)
/**
 * @brief Oboe 实时音频回调（运行于高优先级实时音频线程）
 * 
 * 【高实时铁律】：严禁在此函数内执行任何锁操作（mutex）、动态内存分配（new/malloc）或阻塞系统调用！
 * 所有数据交换完全依托无锁 SPSC 环形缓冲区完成。
 */
oboe::DataCallbackResult FullDuplexAudioIO::onAudioReady(
    oboe::AudioStream *stream,
    void *audioData,
    int32_t numFrames
) {
    auto* float_ptr = static_cast<float*>(audioData);
    int32_t channelCount = stream->getChannelCount();
    int32_t totalSamples = numFrames * channelCount;

    if (stream->getDirection() == oboe::Direction::Input) {
        // --- 录音输入链路 ---
        if (mic_ring_buffer_) {
            if (channelCount <= 1) {
                // 单声道直接推入无锁缓冲区
                mic_ring_buffer_->write(float_ptr, numFrames);
            } else {
                // 若硬件返回立体声麦克风，降混（Downmix）为 16kHz 单声道提供给 VAD/ASR
                float mono_buf[1024];
                int32_t count = std::min(numFrames, 1024);
                for (int32_t i = 0; i < count; ++i) {
                    mono_buf[i] = 0.5f * (float_ptr[i * channelCount] + float_ptr[i * channelCount + 1]);
                }
                mic_ring_buffer_->write(mono_buf, count);
            }
        }
    } else {
        // --- 播音输出链路 ---
        // 关键点：底层声卡请求的 numFrames 数据必须完全被填满。
        // 若 TTS 尚未生成足够采样点（发生 Underrun），必须用 0.0f 静音彻底填平（Zero-fill），
        // 绝对禁止遗留未初始化的内存脏数据，否则扬声器会爆发出剧烈刺耳的白噪声与杂音！
        if (channelCount <= 1) {
            size_t read_count = 0;
            if (spk_ring_buffer_) {
                read_count = spk_ring_buffer_->read(float_ptr, numFrames);
            }
            if (read_count < static_cast<size_t>(numFrames)) {
                std::fill(float_ptr + read_count, float_ptr + numFrames, 0.0f);
            }
        } else {
            // 双声道输出：从单声道环形缓冲区读取后，复制至 L/R 两侧声道
            float mono_buf[1024];
            int32_t count = std::min(numFrames, 1024);
            size_t read_count = 0;
            if (spk_ring_buffer_) {
                read_count = spk_ring_buffer_->read(mono_buf, count);
            }
            if (read_count < static_cast<size_t>(count)) {
                std::fill(mono_buf + read_count, mono_buf + count, 0.0f);
            }
            for (int32_t i = 0; i < count; ++i) {
                float sample = mono_buf[i];
                for (int32_t c = 0; c < channelCount; ++c) {
                    float_ptr[i * channelCount + c] = sample;
                }
            }
            if (totalSamples > count * channelCount) {
                std::fill(float_ptr + count * channelCount, float_ptr + totalSamples, 0.0f);
            }
        }
    }
    return oboe::DataCallbackResult::Continue;
}

/**
 * @brief 音频流异常关闭回调（如蓝牙耳机断开、外接麦克风拔出）
 */
void FullDuplexAudioIO::onErrorAfterClose(oboe::AudioStream *stream, oboe::Result error) {
    if (error == oboe::Result::ErrorDisconnected) {
        // 异步派发至独立子线程重新拉起音频流，防止阻塞当前 Oboe 错误回调
        std::thread([this]() {
            this->RestartStreams();
        }).detach();
    }
}
#else
/**
 * @brief 宿主机模拟/单元测试环境下的音频输入注入
 */
void FullDuplexAudioIO::ProcessMockInputFrames(const float* data, int32_t numFrames) {
    if (mic_ring_buffer_) {
        mic_ring_buffer_->write(data, numFrames);
    }
}

/**
 * @brief 宿主机模拟/单元测试环境下的放音读取（带静音填充）
 */
void FullDuplexAudioIO::ProcessMockOutputFrames(float* data, int32_t numFrames) {
    size_t read_count = 0;
    if (spk_ring_buffer_) {
        read_count = spk_ring_buffer_->read(data, numFrames);
    }
    if (read_count < static_cast<size_t>(numFrames)) {
        std::fill(data + read_count, data + numFrames, 0.0f);
    }
}
#endif

size_t FullDuplexAudioIO::SimulateMicInput(const float* data, size_t count) {
    if (!mic_ring_buffer_) return 0;
    return mic_ring_buffer_->write(data, count);
}

size_t FullDuplexAudioIO::SimulateSpeakerOutput(float* dest, size_t count) {
    if (!spk_ring_buffer_) return 0;
    return spk_ring_buffer_->read(dest, count);
}

} // namespace audio_agent

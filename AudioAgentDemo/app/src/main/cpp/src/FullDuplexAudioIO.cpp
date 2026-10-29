#include "FullDuplexAudioIO.hpp"
#include <algorithm>
#include <thread>
#include <chrono>

namespace audio_agent {

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

bool FullDuplexAudioIO::StartStreams() {
    if (is_running_.load(std::memory_order_acquire)) {
        return true;
    }

#if defined(AUDIO_AGENT_USE_OBOE)
    // 1. Configure Input Stream (16kHz, VoiceCommunication for hardware DSP AEC/NS/AGC)
    oboe::AudioStreamBuilder inBuilder;
    inBuilder.setDirection(oboe::Direction::Input)
             ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
             ->setSharingMode(oboe::SharingMode::Shared)
             ->setFormat(oboe::AudioFormat::Float)
             ->setChannelCount(oboe::ChannelCount::Mono)
             ->setSampleRate(mic_sample_rate_)
             ->setInputPreset(oboe::InputPreset::VoiceCommunication) // Hardware AEC/NS/AGC
             ->setDataCallback(this)
             ->setErrorCallback(this);

    oboe::Result res = inBuilder.openStream(input_stream_);
    if (res != oboe::Result::OK) {
        return false;
    }

    // 2. Configure Output Stream (24kHz, VoiceCommunication usage for AEC reference)
    oboe::AudioStreamBuilder outBuilder;
    outBuilder.setDirection(oboe::Direction::Output)
              ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
              ->setSharingMode(oboe::SharingMode::Exclusive)
              ->setFormat(oboe::AudioFormat::Float)
              ->setChannelCount(oboe::ChannelCount::Mono)
              ->setSampleRate(spk_sample_rate_)
              ->setUsage(oboe::Usage::VoiceCommunication) // Paired with input stream for DSP AEC reference loop
              ->setDataCallback(this)
              ->setErrorCallback(this);

    res = outBuilder.openStream(output_stream_);
    if (res != oboe::Result::OK) {
        input_stream_->close();
        input_stream_.reset();
        return false;
    }

    output_stream_->requestStart();
    input_stream_->requestStart();
#endif

    is_running_.store(true, std::memory_order_release);
    return true;
}

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

bool FullDuplexAudioIO::RestartStreams() {
    bool expected = false;
    if (!is_restarting_.compare_exchange_strong(expected, true)) {
        return false; // Already restarting
    }

    StopStreams();
    // Allow hardware audio HAL routing to settle (under 50ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    bool success = StartStreams();

    is_restarting_.store(false, std::memory_order_release);
    return success;
}

#if defined(AUDIO_AGENT_USE_OBOE)
oboe::DataCallbackResult FullDuplexAudioIO::onAudioReady(
    oboe::AudioStream *stream,
    void *audioData,
    int32_t numFrames
) {
    auto* float_ptr = static_cast<float*>(audioData);
    int32_t channelCount = stream->getChannelCount();
    int32_t totalSamples = numFrames * channelCount;

    if (stream->getDirection() == oboe::Direction::Input) {
        // High priority audio thread: zero mutex, zero malloc!
        if (mic_ring_buffer_) {
            if (channelCount <= 1) {
                mic_ring_buffer_->write(float_ptr, numFrames);
            } else {
                // Downmix stereo mic to mono (16kHz mono) for VAD/ASR
                float mono_buf[1024];
                int32_t count = std::min(numFrames, 1024);
                for (int32_t i = 0; i < count; ++i) {
                    mono_buf[i] = 0.5f * (float_ptr[i * channelCount] + float_ptr[i * channelCount + 1]);
                }
                mic_ring_buffer_->write(mono_buf, count);
            }
        }
    } else {
        // Read synthesized TTS frames from speaker ring buffer.
        // Crucial: ALL channels up to totalSamples MUST be completely written or zero-filled,
        // otherwise uninitialized memory in channel 2 blasts loud static/white noise.
        if (channelCount <= 1) {
            size_t read_count = 0;
            if (spk_ring_buffer_) {
                read_count = spk_ring_buffer_->read(float_ptr, numFrames);
            }
            if (read_count < static_cast<size_t>(numFrames)) {
                std::fill(float_ptr + read_count, float_ptr + numFrames, 0.0f);
            }
        } else {
            // Stereo output: read mono from ring buffer and duplicate to L & R
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

void FullDuplexAudioIO::onErrorAfterClose(oboe::AudioStream *stream, oboe::Result error) {
    if (error == oboe::Result::ErrorDisconnected) {
        // Handle route changes / bluetooth disconnect within 50ms as specified in Section 6.3
        std::thread([this]() {
            this->RestartStreams();
        }).detach();
    }
}
#else
void FullDuplexAudioIO::ProcessMockInputFrames(const float* data, int32_t numFrames) {
    if (mic_ring_buffer_) {
        mic_ring_buffer_->write(data, numFrames);
    }
}

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

#pragma once

#include "LockFreeRingBuffer.hpp"
#include "EngineInterfaces.hpp"
#include <memory>
#include <atomic>
#include <functional>

#if defined(AUDIO_AGENT_USE_OBOE)
#include <oboe/Oboe.h>
#endif

namespace audio_agent {

/**
 * Full Duplex Audio IO Engine.
 * Manages concurrent Microphone (16kHz) recording and Speaker (24kHz) playback.
 * Connects Oboe hardware DSP AEC with lock-free SPSC ring buffers.
 */
#if defined(AUDIO_AGENT_USE_OBOE)
class FullDuplexAudioIO : public oboe::AudioStreamDataCallback,
                          public oboe::AudioStreamErrorCallback {
#else
class FullDuplexAudioIO {
#endif
public:
    using MicDataListener = std::function<void(const float* data, size_t count)>;

    FullDuplexAudioIO(
        std::shared_ptr<LockFreeSPSCRingBuffer<float>> mic_ring_buffer,
        std::shared_ptr<LockFreeSPSCRingBuffer<float>> spk_ring_buffer,
        int32_t mic_sample_rate = DEFAULT_MIC_SAMPLE_RATE,
        int32_t spk_sample_rate = DEFAULT_SPK_SAMPLE_RATE
    );

    virtual ~FullDuplexAudioIO();

    // Start full-duplex streams
    bool StartStreams();

    // Stop streams
    void StopStreams();

    // Restart streams (used when audio route changes or headset disconnected)
    bool RestartStreams();

    bool IsRunning() const { return is_running_.load(std::memory_order_acquire); }

    int32_t GetMicSampleRate() const { return mic_sample_rate_; }
    int32_t GetSpkSampleRate() const { return spk_sample_rate_; }

    // Test/simulation helper: inject mic frames into input pipeline
    size_t SimulateMicInput(const float* data, size_t count);

    // Test/simulation helper: read speaker output frames
    size_t SimulateSpeakerOutput(float* dest, size_t count);

#if defined(AUDIO_AGENT_USE_OBOE)
    // Oboe AudioStreamDataCallback
    oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream *stream,
        void *audioData,
        int32_t numFrames
    ) override;

    // Oboe AudioStreamErrorCallback (Section 6.3)
    void onErrorAfterClose(
        oboe::AudioStream *stream,
        oboe::Result error
    ) override;
#else
    // Host mock callbacks with identical semantics
    void ProcessMockInputFrames(const float* data, int32_t numFrames);
    void ProcessMockOutputFrames(float* data, int32_t numFrames);
#endif

private:
    std::shared_ptr<LockFreeSPSCRingBuffer<float>> mic_ring_buffer_;
    std::shared_ptr<LockFreeSPSCRingBuffer<float>> spk_ring_buffer_;
    const int32_t mic_sample_rate_;
    const int32_t spk_sample_rate_;
    std::atomic<bool> is_running_{false};
    std::atomic<bool> is_restarting_{false};

#if defined(AUDIO_AGENT_USE_OBOE)
    std::shared_ptr<oboe::AudioStream> input_stream_;
    std::shared_ptr<oboe::AudioStream> output_stream_;
#endif
};

} // namespace audio_agent

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <ostream>

namespace audio_agent {

/**
 * Common Audio Configurations matching Whitepaper Section 1.3 & 2.2
 */
constexpr int32_t DEFAULT_MIC_SAMPLE_RATE = 16000; // 16kHz for Silero VAD & Sherpa-onnx Zipformer ASR
constexpr int32_t DEFAULT_SPK_SAMPLE_RATE = 24000; // 24kHz for Kokoro / Piper TTS
constexpr int32_t VAD_FRAME_SIZE_SAMPLES = 512;    // 32ms at 16kHz
constexpr float VAD_SILENCE_THRESHOLD_SEC = 0.3f;  // 300ms endpoint cutoff

/**
 * Finite State Machine for Full-Duplex AI Audio Agent
 * IDLE -> LISTENING -> THINKING -> SPEAKING (and Barge-in back to LISTENING)
 */
enum class AgentState {
    IDLE = 0,
    LISTENING = 1,
    THINKING = 2,
    SPEAKING = 3
};

inline const char* AgentStateToString(AgentState state) {
    switch (state) {
        case AgentState::IDLE: return "IDLE";
        case AgentState::LISTENING: return "LISTENING";
        case AgentState::THINKING: return "THINKING";
        case AgentState::SPEAKING: return "SPEAKING";
        default: return "UNKNOWN";
    }
}

/**
 * Speech Perception Interface (VAD + Streaming ASR)
 */
class ISpeechPerceptionEngine {
public:
    virtual ~ISpeechPerceptionEngine() = default;

    // Push mic audio chunk (16kHz float mono). Returns true if human voice is actively detected by VAD
    virtual bool AcceptAudioChunk(const float* samples, int32_t count) = 0;

    // Check if user has stopped speaking (silence > 300ms endpoint)
    virtual bool IsEndpointDetected() = 0;

    // Get current ASR recognized text (incremental or final)
    virtual std::string GetRecognizedText(bool is_final = false) = 0;

    // Reset ASR stream and VAD state
    virtual void Reset() = 0;
};

/**
 * Speech Synthesis Interface (Streaming TTS)
 */
class ISpeechSynthesisEngine {
public:
    virtual ~ISpeechSynthesisEngine() = default;

    using AudioChunkCallback = std::function<int32_t(const float* samples, int32_t count, float progress)>;

    /**
     * Synthesizes text chunk-by-chunk. Invokes callback as soon as audio PCM frames are generated.
     * If callback returns 0, synthesis must abort immediately (Barge-in).
     */
    virtual bool SynthesizeStreaming(const std::string& text, AudioChunkCallback callback) = 0;

    virtual int32_t GetSampleRate() const = 0;
};

/**
 * LLM Inference Interface (MNN-LLM)
 */
class ILlmEngine {
public:
    virtual ~ILlmEngine() = default;

    // Streaming response writing generated tokens into out_stream (e.g. StreamingSentenceChunker)
    virtual void ResponseStream(const std::string& query, std::ostream* out_stream) = 0;

    // Truncate/Rollback KV Cache to token count
    virtual void RollbackTokens(size_t tokens_to_keep) = 0;

    // Pre-warm prefix KV cache with System Prompt
    virtual bool WarmupPrefix(const std::string& system_prompt) = 0;
};

} // namespace audio_agent

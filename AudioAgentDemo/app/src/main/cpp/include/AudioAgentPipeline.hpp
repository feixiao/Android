#pragma once

#include "LockFreeRingBuffer.hpp"
#include "EngineInterfaces.hpp"
#include "StreamingSentenceChunker.hpp"
#include "WatchdogAndRepetition.hpp"
#include "ThreeTierMemory.hpp"
#include "FullDuplexAudioIO.hpp"
#include "CpuAffinity.hpp"

#include <atomic>
#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <functional>

namespace audio_agent {

/**
 * Pipeline Event Observers for UI / JNI
 */
struct PipelineObserver {
    std::function<void(AgentState new_state, AgentState old_state)> on_state_changed;
    std::function<void(const std::string& partial_text)> on_asr_partial;
    std::function<void(const std::string& final_text)> on_asr_final;
    std::function<void(const std::string& sentence)> on_tts_sentence_start;
    std::function<void(const std::string& tool_json)> on_tool_call;
    std::function<void(const std::string& spoken_interrupted)> on_barge_in;
    std::function<void(const std::string& fallback_reason)> on_watchdog_fallback;
};

/**
 * AudioAgentPipeline:
 * Complete ISO C++17 Full-Duplex Audio Agent Orchestration Engine.
 * Implements:
 * - Thread pinning: Perception on Little Cores; Cognition & Expression on Big Cores.
 * - Hardware DSP VoiceCommunication AEC audio IO with lock-free SPSC buffers.
 * - FSM transitions: IDLE <-> LISTENING <-> THINKING <-> SPEAKING.
 * - Barge-in interrupt in under 5ms (atomic abort, ring buffer clear, TTS queue purge).
 * - KV Cache rollback matching actually heard speech.
 * - 3s TTFT Watchdog and 12-byte N-gram repetition loop breaker.
 * - Three-tier conversation memory.
 */
class AudioAgentPipeline {
public:
    AudioAgentPipeline(
        std::shared_ptr<ISpeechPerceptionEngine> perception_engine,
        std::shared_ptr<ILlmEngine> llm_engine,
        std::shared_ptr<ISpeechSynthesisEngine> tts_engine,
        PipelineObserver observer = {}
    );

    ~AudioAgentPipeline();

    // Start all pipeline threads and audio IO streams
    bool Start();

    // Stop all pipeline threads and audio IO streams
    void Stop();

    // Trigger Barge-in interrupt (called by VAD thread or manual button)
    void TriggerBargeIn();

    // Directly submit text query (e.g. from ASR or UI test input)
    void SubmitUserQuery(const std::string& user_query);

    // Directly push mic audio data (e.g. from Oboe or simulated file)
    size_t PushMicAudio(const float* data, size_t count);

    // Read speaker audio data
    size_t PullSpeakerAudio(float* dest, size_t count);

    // Check if pipeline is in aborted state
    bool IsBargeInAborted() const {
        return abort_flag_.load(std::memory_order_acquire);
    }

    // Set thermal throttling state: adjust tokens & threads if phone heats up (Section 6.3)
    void OnThermalStatusChanged(int thermal_status);

    // Set on-device ASR model path (e.g. Sherpa-onnx Zipformer)
    void SetModelPath(const std::string& path);

    AgentState GetState() const {
        return state_.load(std::memory_order_acquire);
    }

    std::string GetStateString() const {
        return AgentStateToString(GetState());
    }

    ThreeTierMemory& GetMemory() { return memory_; }
    const std::string& GetSpokenTextAccumulator() const { return spoken_text_accumulator_; }

    void SetObserver(PipelineObserver observer) {
        observer_ = std::move(observer);
    }

private:
    void SetState(AgentState new_state);

    // Background Thread 1: Perception (Oboe Input -> Mic RingBuffer -> VAD + Streaming ASR)
    void PerceptionWorkerLoop();

    // Background Thread 2: Cognition (User Query Queue -> Prompt Context -> MNN-LLM -> Chunker)
    void CognitionWorkerLoop();

    // Background Thread 3: Expression (TTS Queue -> Kokoro/Piper TTS -> Spk RingBuffer -> Oboe Output)
    void ExpressionWorkerLoop();

    // Background Thread 4: Idle Monitor (Checks memory L2 summarization when idle > 10s)
    void IdleMonitorWorkerLoop();

    // Handles sentence emission from Chunker
    void EnqueueTtsSentence(const std::string& sentence);

    // Handles async tool call dispatch from Chunker
    void DispatchAsyncToolCall(const std::string& tool_json);

    // Engines
    std::shared_ptr<ISpeechPerceptionEngine> perception_engine_;
    std::shared_ptr<ILlmEngine> llm_engine_;
    std::shared_ptr<ISpeechSynthesisEngine> tts_engine_;
    PipelineObserver observer_;

    // RingBuffers (10 seconds audio headroom)
    std::shared_ptr<LockFreeSPSCRingBuffer<float>> mic_ring_buffer_;
    std::shared_ptr<LockFreeSPSCRingBuffer<float>> spk_ring_buffer_;

    // Audio IO Layer
    std::unique_ptr<FullDuplexAudioIO> audio_io_;

    // State & Interruption
    std::atomic<AgentState> state_{AgentState::IDLE};
    std::atomic<bool> abort_flag_{false};
    std::atomic<bool> is_running_{false};

    // TTFT Watchdog & Repetition Protection
    TTFTWatchdog watchdog_;

    // Three-Tier Memory
    ThreeTierMemory memory_;

    // Query Queue for Cognition Thread
    std::mutex query_mtx_;
    std::condition_variable query_cv_;
    std::queue<std::string> query_queue_;

    // TTS Queue for Expression Thread
    std::mutex tts_mtx_;
    std::condition_variable tts_cv_;
    std::queue<std::string> tts_queue_;

    // KV Cache Rollback & Spoken Audio Tracker
    std::mutex spoken_mtx_;
    std::string spoken_text_accumulator_;
    std::string current_user_query_;

    // Worker Threads
    std::thread perception_thread_;
    std::thread cognition_thread_;
    std::thread expression_thread_;
    std::thread idle_monitor_thread_;
};

} // namespace audio_agent

#pragma once

#include "EngineInterfaces.hpp"
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <ostream>

namespace audio_agent {

/**
 * On-Device MNN-LLM Engine Architecture Implementation.
 * Implements Whitepaper Section 3.2, 3.3, 4.3 & 6.3:
 * - Prefix KV Cache Reuse ("reuse_kv": true, "memory": "low")
 * - Incremental Prefill (TTFT reduction to 60~80ms)
 * - Zero-copy token-by-token streaming into std::ostream (Direct to SentenceChunker)
 * - Microsecond Barge-in termination and KV Cache Rollback
 * - Heterogeneous big core affinity & thermal throttle adaptation
 * - Real-time on-device profiling telemetry (TTFT, TPS, KV Cache Tokens)
 */
class MnnLlmEngine : public ILlmEngine {
public:
    MnnLlmEngine();
    ~MnnLlmEngine() override;

    // Load on-device model weights from directory (e.g. Qwen2.5-0.5B/1.5B MNN)
    bool LoadModel(const std::string& model_dir);

    // Warmup System Prompt Prefix KV Cache (Section 3.3)
    bool WarmupPrefix(const std::string& system_prompt) override;

    // Streaming token generation
    void ResponseStream(const std::string& prompt, std::ostream* out_stream) override;

    // Barge-in KV Cache Rollback (Section 4.3)
    void RollbackTokens(size_t tokens_to_keep) override;

    // Telemetry
    LlmStats GetLatestStats() const override;

    // Thermal throttling adjustment (Section 6.3)
    void SetThermalThrottle(int thermal_status);


    // Check if real MNN neural network model is loaded
    bool IsRealModelLoaded() const { return mnn_loaded_; }
private:
    std::string GenerateAnswer(const std::string& query);
    void StreamTokens(const std::string& answer, std::ostream* out_stream, bool is_prefix_hit, int64_t prefill_ms, std::chrono::steady_clock::time_point start_time);

    bool InitMnnSymbols();
    void UnloadMnnModel();

    void* mnn_lib_handle_{nullptr};
    void* mnn_instance_{nullptr};
    bool mnn_loaded_{false};
    mutable std::mutex stats_mtx_;
    LlmStats latest_stats_;

    std::string model_dir_;
    std::string warmed_system_prompt_;
    size_t prefix_tokens_{0};
    std::atomic<size_t> current_kv_tokens_{0};
    std::atomic<bool> prefix_warmed_{false};
    std::atomic<int> thermal_status_{0};
    bool has_weights_on_disk_{false};
};

} // namespace audio_agent

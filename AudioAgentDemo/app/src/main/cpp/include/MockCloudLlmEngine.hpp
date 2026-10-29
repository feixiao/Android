#pragma once

#include "EngineInterfaces.hpp"
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <ostream>
#include <chrono>
#include <thread>

namespace audio_agent {

/**
 * Cloud LLM API Streaming Mock Implementation (A-Side Mock).
 * Used for comparative research against on-device MNN-LLM (B-Side):
 * - Simulates WAN network RTT delay (~250-350ms)
 * - Simulates SSE streaming chunks (bursty delivery)
 * - Demonstrates contrast: Cloud TTFT (~320ms, network-bound) vs Local MNN-LLM TTFT (~65ms, zero-network)
 */
class MockCloudLlmEngine : public ILlmEngine {
public:
    MockCloudLlmEngine() {
        latest_stats_.engine_name = "Cloud-API (Mocked SSE)";
    }
    ~MockCloudLlmEngine() override = default;

    bool WarmupPrefix(const std::string& /*system_prompt*/) override {
        // Cloud APIs are typically stateless or incur session handshake overhead
        return false;
    }

    void RollbackTokens(size_t /*tokens_to_keep*/) override {
        // Server-side LLM generated tokens cannot be rolled back locally
    }

    LlmStats GetLatestStats() const override {
        std::lock_guard<std::mutex> lock(stats_mtx_);
        return latest_stats_;
    }

    void ResponseStream(const std::string& prompt, std::ostream* out_stream) override {
        if (!out_stream) return;

        auto start_time = std::chrono::steady_clock::now();

        // Extract user query
        std::string query = prompt;
        size_t pos = prompt.rfind("<|im_start|>user\n");
        if (pos != std::string::npos) {
            size_t start = pos + std::string("<|im_start|>user\n").length();
            size_t end = prompt.find("<|im_end|>", start);
            if (end != std::string::npos) query = prompt.substr(start, end - start);
            else query = prompt.substr(start);
        }

        // Simulate Cloud WAN Network Round-Trip Time (RTT: 280ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(280));

        std::string answer = "好的，我是通过云端大模型接口生成的回答。关于您问的“" + query + "”，从全局大模型知识库来看，建议结合实际场景综合评估！";

        // Deliver tokens in bursty SSE chunks (3~5 characters per TCP packet)
        size_t offset = 0;
        size_t total_tokens = 0;
        bool first_chunk = true;
        int64_t ttft_ms = 280;

        auto gen_start = std::chrono::steady_clock::now();

        while (offset < answer.size()) {
            if (!out_stream->good()) break; // Barge-in cancellation

            size_t chunk_len = std::min<size_t>(12, answer.size() - offset);
            std::string chunk = answer.substr(offset, chunk_len);
            offset += chunk_len;

            *out_stream << chunk;
            out_stream->flush();
            total_tokens += 3;

            if (first_chunk) {
                first_chunk = false;
                auto now = std::chrono::steady_clock::now();
                ttft_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
            }

            // Network packet arrival jitter (40-60ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(45));
        }

        auto gen_end = std::chrono::steady_clock::now();
        int64_t dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(gen_end - gen_start).count();
        float tps = dur_ms > 0 ? (static_cast<float>(total_tokens) * 1000.0f / dur_ms) : 0.0f;

        {
            std::lock_guard<std::mutex> lock(stats_mtx_);
            latest_stats_.prefill_latency_ms = 280;
            latest_stats_.ttft_ms = ttft_ms;
            latest_stats_.tokens_per_second = tps;
            latest_stats_.kv_cache_tokens = 0; // Stateless
            latest_stats_.generated_tokens = total_tokens;
            latest_stats_.is_prefix_hit = false;
            latest_stats_.engine_name = "Cloud-API (Mocked SSE)";
        }
    }

private:
    mutable std::mutex stats_mtx_;
    LlmStats latest_stats_;
};

} // namespace audio_agent

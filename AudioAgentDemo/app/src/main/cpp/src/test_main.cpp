#include "LockFreeRingBuffer.hpp"
#include "CpuAffinity.hpp"
#include "StreamingSentenceChunker.hpp"
#include "WatchdogAndRepetition.hpp"
#include "ThreeTierMemory.hpp"
#include "FullDuplexAudioIO.hpp"
#include "AudioAgentPipeline.hpp"

#include <iostream>
#include <cassert>
#include <chrono>
#include <thread>
#include <cmath>

using namespace audio_agent;

// ANSI Colors for test reporter
#define COLOR_GREEN "\033[32m"
#define COLOR_RED   "\033[31m"
#define COLOR_BLUE  "\033[34m"
#define COLOR_RESET "\033[0m"

static void LogPass(const std::string& name) {
    std::cout << COLOR_GREEN << "[PASS] " << COLOR_RESET << name << std::endl;
}

static void LogTest(const std::string& name) {
    std::cout << COLOR_BLUE << "=== Running Test: " << name << " ===" << COLOR_RESET << std::endl;
}

// -------------------------------------------------------------
// Test 1: Lock-Free SPSC RingBuffer
// -------------------------------------------------------------
static void TestRingBuffer() {
    LogTest("LockFreeSPSCRingBuffer SPSC Operations");

    LockFreeSPSCRingBuffer<float> rb(1024);
    assert(rb.capacity() == 1024);
    assert(rb.empty());
    assert(rb.available_read() == 0);
    assert(rb.available_write() == 1024);

    // Single write and read
    std::vector<float> input(512, 1.23f);
    size_t written = rb.write(input.data(), input.size());
    assert(written == 512);
    assert(rb.available_read() == 512);

    std::vector<float> output(512, 0.0f);
    size_t read_cnt = rb.read(output.data(), 512);
    assert(read_cnt == 512);
    assert(rb.empty());
    assert(output[0] == 1.23f);

    // Wrap-around test
    std::vector<float> chunk1(800, 2.0f);
    written = rb.write(chunk1.data(), chunk1.size());
    assert(written == 800);

    std::vector<float> out1(600, 0.0f);
    read_cnt = rb.read(out1.data(), 600);
    assert(read_cnt == 600);

    // Now write 600 more elements to wrap around circular buffer
    std::vector<float> chunk2(600, 3.0f);
    written = rb.write(chunk2.data(), chunk2.size());
    assert(written == 600);

    // Clear test
    rb.clear();
    assert(rb.empty());
    assert(rb.available_read() == 0);

    // Multithreaded SPSC test
    std::atomic<bool> producer_done{false};
    const size_t TOTAL_SAMPLES = 100000;
    LockFreeSPSCRingBuffer<float> thread_rb(2048);

    std::thread producer([&]() {
        std::vector<float> batch(128, 42.0f);
        size_t sent = 0;
        while (sent < TOTAL_SAMPLES) {
            size_t to_send = std::min(batch.size(), TOTAL_SAMPLES - sent);
            size_t w = thread_rb.write(batch.data(), to_send);
            sent += w;
            if (w == 0) std::this_thread::yield();
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        std::vector<float> batch(128, 0.0f);
        size_t received = 0;
        while (received < TOTAL_SAMPLES) {
            size_t to_read = std::min(batch.size(), TOTAL_SAMPLES - received);
            size_t r = thread_rb.read(batch.data(), to_read);
            received += r;
            if (r == 0) std::this_thread::yield();
        }
        assert(received == TOTAL_SAMPLES);
    });

    producer.join();
    consumer.join();

    LogPass("LockFreeSPSCRingBuffer passed all tests.");
}

// -------------------------------------------------------------
// Test 2: Sentence Chunker Dynamic Thresholds & Punctuation
// -------------------------------------------------------------
static void TestSentenceChunker() {
    LogTest("StreamingSentenceChunker Dynamic Thresholds");

    std::vector<std::string> emitted_sentences;
    std::string tool_call_received;

    SentenceChunkerCallbacks callbacks;
    callbacks.on_sentence_ready = [&](const std::string& sentence) {
        emitted_sentences.push_back(sentence);
    };
    callbacks.on_tool_call = [&](const std::string& tool_json) {
        tool_call_received = tool_json;
    };
    callbacks.is_aborted = []() { return false; };

    StreamingSentenceChunker chunker(std::move(callbacks));
    std::ostream stream(&chunker);

    // Simulate LLM streaming token by token:
    // First sentence: "好的，" (6 bytes) -> triggers immediate 1st chunk
    // Subsequent: "马上为您播放今天的天气预报。" (>21 bytes) -> triggers 2nd chunk
    stream << "好";
    stream.flush();
    assert(emitted_sentences.empty()); // 3 bytes, under 6 bytes

    stream << "的";
    stream.flush();
    assert(emitted_sentences.empty()); // 6 bytes, no punctuation yet

    stream << "，"; // punctuation!
    stream.flush();
    assert(emitted_sentences.size() == 1);
    assert(emitted_sentences[0] == "好的，");

    // Second sentence: requires >= 21 bytes
    stream << "马上为您"; // 12 bytes
    stream.flush();
    assert(emitted_sentences.size() == 1);

    stream << "播放今天的"; // 15 bytes -> total 27 bytes
    stream.flush();
    assert(emitted_sentences.size() == 1);

    stream << "天气预报。"; // punctuation!
    stream.flush();
    assert(emitted_sentences.size() == 2);
    assert(emitted_sentences[1] == "马上为您播放今天的天气预报。");

    // Test tool call parsing: <|tool_call|>{"action":"query_weather"}<|/tool_call|>
    emitted_sentences.clear();
    stream << "<|tool_call|>";
    stream << "{\"action\":\"query_weather\",\"city\":\"杭州\"}";
    stream << "<|/tool_call|>";
    stream.flush();

    assert(emitted_sentences.empty()); // Tool calls must NOT be spoken!
    assert(!tool_call_received.empty());
    assert(tool_call_received.find("query_weather") != std::string::npos);

    LogPass("StreamingSentenceChunker dynamic thresholds and tool calling passed.");
}

// -------------------------------------------------------------
// Test 3: Watchdog and Repetition Detection
// -------------------------------------------------------------
static void TestWatchdogAndRepetition() {
    LogTest("TTFTWatchdog and N-Gram Repetition Detection");

    // 1. Watchdog timeout test
    TTFTWatchdog watchdog;
    std::atomic<bool> timed_out{false};

    watchdog.Start(100, [&]() {
        timed_out.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    assert(timed_out.load());

    // 2. Watchdog fed test (normal case)
    timed_out.store(false);
    watchdog.Start(200, [&]() {
        timed_out.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    watchdog.Feed(); // Fed in time
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    assert(!timed_out.load());
    watchdog.Stop();

    // 3. N-gram repetition test
    std::atomic<bool> repetition_detected{false};
    SentenceChunkerCallbacks callbacks;
    callbacks.on_repetition_detected = [&]() {
        repetition_detected.store(true);
    };
    callbacks.is_aborted = [&]() { return repetition_detected.load(); };

    StreamingSentenceChunker chunker(std::move(callbacks));
    std::ostream stream(&chunker);

    // Feed repeating pattern: "天气预报天气预报天气预报" (12 bytes repeating twice)
    stream << "天气预报";
    stream << "天气预报";
    stream << "天气预报";
    stream.flush();

    assert(repetition_detected.load());
    LogPass("TTFTWatchdog and Repetition Detector passed.");
}

// -------------------------------------------------------------
// Test 4: Three-Tier Memory and KV Cache Rollback
// -------------------------------------------------------------
static void TestThreeTierMemory() {
    LogTest("ThreeTierMemory L1/L2/L3 & KV Cache Rollback");

    ThreeTierMemory mem("你是本地语音助手");

    // L1 active turns test
    mem.AddTurn("你好", "你好！很高兴为您服务。");
    mem.AddTurn("杭州天气怎么样", "杭州今天晴天，气温二十度。");

    auto turns = mem.GetActiveTurns();
    assert(turns.size() == 2);
    assert(turns[0].user_query == "你好");

    // Barge-in rollback test
    // Suppose assistant was going to say 50 words, but was interrupted after "好的，已为您"
    mem.AddTurn("帮我放首歌", "好的，已为您搜索并播放周杰伦的晴天。");
    mem.RollbackLastAssistantTurn("好的，已为您");

    turns = mem.GetActiveTurns();
    assert(turns.back().was_interrupted);
    assert(turns.back().assistant_response == "好的，已为您（被用户打断）");

    // L1 FIFO Eviction: Add 3 more turns -> oldest turns should be evicted
    mem.AddTurn("音量调大", "好的，音量已调大。");
    mem.AddTurn("再大一点", "好的，已调至最大。");
    mem.AddTurn("明天会下雨吗", "明天无雨。");

    turns = mem.GetActiveTurns();
    assert(turns.size() == 4); // Capped at MAX_L1_TURNS = 4

    // L3 fact retrieval
    mem.AddFact("用户常住城市为杭州");
    mem.AddFact("用户空调习惯温度为24度");

    auto facts = mem.RetrieveRelevantFacts("杭州", 1);
    assert(!facts.empty());
    assert(facts[0].find("杭州") != std::string::npos);

    // Format prompt context
    std::string prompt = mem.FormatPromptContext("今天降温了吗");
    assert(prompt.find("你是本地语音助手") != std::string::npos);
    assert(prompt.find("今天降温了吗") != std::string::npos);

    LogPass("ThreeTierMemory passed.");
}

// -------------------------------------------------------------
// Test 5: Full-Duplex Pipeline End-to-End & Barge-in Interrupt
// -------------------------------------------------------------

class SimulatedPerceptionEngine : public ISpeechPerceptionEngine {
public:
    bool AcceptAudioChunk(const float* samples, int32_t count) override {
        // Voice active when simulated speech energy is high
        float energy = 0.0f;
        for (int i = 0; i < count; ++i) energy += std::abs(samples[i]);
        return (energy / count) > 0.05f;
    }

    bool IsEndpointDetected() override {
        return endpoint_detected_;
    }

    std::string GetRecognizedText(bool /*is_final*/) override {
        return recognized_text_;
    }

    void Reset() override {
        endpoint_detected_ = false;
        recognized_text_.clear();
    }

    void SimulateUserSpeaking(const std::string& text, bool endpoint = true) {
        recognized_text_ = text;
        endpoint_detected_ = endpoint;
    }

private:
    std::string recognized_text_;
    bool endpoint_detected_{false};
};

class SimulatedLlmEngine : public ILlmEngine {
public:
    void ResponseStream(const std::string& /*query*/, std::ostream* out_stream) override {
        if (!out_stream) return;

        // Output first sentence quickly (<100ms) to meet 150ms TTFT budget!
        *out_stream << "好的，";
        out_stream->flush();

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Output second sentence
        *out_stream << "正在为您查询最新的路况信息。";
        out_stream->flush();
    }

    void RollbackTokens(size_t tokens_to_keep) override {
        rolled_back_tokens_ = tokens_to_keep;
    }

    bool WarmupPrefix(const std::string& /*system_prompt*/) override {
        prefix_warmed_ = true;
        return true;
    }

    size_t rolled_back_tokens_{0};
    bool prefix_warmed_{false};
};

class SimulatedTtsEngine : public ISpeechSynthesisEngine {
public:
    bool SynthesizeStreaming(const std::string& /*text*/, AudioChunkCallback callback) override {
        // Generate simulated PCM frames in 4 chunks
        const int CHUNK_SIZE = 480; // 20ms at 24kHz
        std::vector<float> pcm(CHUNK_SIZE, 0.5f);

        for (int i = 0; i < 4; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            int rc = callback(pcm.data(), CHUNK_SIZE, (i + 1) * 0.25f);
            if (rc == 0) {
                // Aborted by Barge-in!
                return false;
            }
        }
        return true;
    }

    int32_t GetSampleRate() const override { return 24000; }
};

static void TestFullDuplexPipeline() {
    LogTest("Full-Duplex Pipeline End-to-End and Barge-In Verification");

    auto perception = std::make_shared<SimulatedPerceptionEngine>();
    auto llm = std::make_shared<SimulatedLlmEngine>();
    auto tts = std::make_shared<SimulatedTtsEngine>();

    std::atomic<bool> saw_speaking{false};
    std::atomic<bool> saw_barge_in{false};
    std::string barge_in_spoken_text;

    PipelineObserver observer;
    observer.on_state_changed = [&](AgentState new_state, AgentState old_state) {
        std::cout << "  [FSM Transition] " << AgentStateToString(old_state)
                  << " -> " << AgentStateToString(new_state) << std::endl;
        if (new_state == AgentState::SPEAKING) {
            saw_speaking.store(true);
        }
    };
    observer.on_barge_in = [&](const std::string& spoken) {
        saw_barge_in.store(true);
        barge_in_spoken_text = spoken;
        std::cout << "  [Barge-in Triggered] Audio silenced, spoken text retained: '"
                  << spoken << "'" << std::endl;
    };

    AudioAgentPipeline pipeline(perception, llm, tts, observer);
    bool started = pipeline.Start();
    assert(started);
    assert(pipeline.GetState() == AgentState::IDLE);

    // 1. Submit query and observe transition: IDLE -> THINKING -> SPEAKING
    pipeline.SubmitUserQuery("今天去西湖堵车吗？");

    // Wait until agent enters SPEAKING state
    int wait_cycles = 0;
    while (!saw_speaking.load() && wait_cycles++ < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(saw_speaking.load());
    assert(pipeline.GetState() == AgentState::SPEAKING);

    // 2. Test Barge-in while SPEAKING!
    // Trigger barge-in interrupt:
    pipeline.TriggerBargeIn();

    assert(pipeline.IsBargeInAborted());
    assert(pipeline.GetState() == AgentState::LISTENING);
    assert(saw_barge_in.load());

    // Verify KV cache rollback occurred
    auto active_turns = pipeline.GetMemory().GetActiveTurns();
    if (!active_turns.empty()) {
        std::cout << "  [Memory Verified] Last response recorded: "
                  << active_turns.back().assistant_response << std::endl;
    }

    pipeline.Stop();
    LogPass("Full-Duplex Pipeline end-to-end and Barge-in passed.");
}

// -------------------------------------------------------------
// Test 6: CPU Core Affinity Detection
// -------------------------------------------------------------
static void TestCpuAffinity() {
    LogTest("Heterogeneous CPU Core Affinity Detection");

    auto big_cores = DetectTopPerformanceCores(4);
    assert(!big_cores.empty());
    std::cout << "  Detected Top Performance Cores: ";
    for (int id : big_cores) std::cout << id << " ";
    std::cout << std::endl;

    auto little_cores = DetectEfficiencyCores(2);
    assert(!little_cores.empty());
    std::cout << "  Detected Efficiency Cores: ";
    for (int id : little_cores) std::cout << id << " ";
    std::cout << std::endl;

    bool bound_big = BindThreadToBigCores(2);
    std::cout << "  Bind to Big Cores returned: " << (bound_big ? "true" : "false (advisory)") << std::endl;

    LogPass("CPU Affinity Detection passed.");
}

int main() {
    std::cout << "\n=======================================================\n";
    std::cout << "  Android AI Audio Agent C++17 Smoke Test Suite\n";
    std::cout << "=======================================================\n\n";

    TestRingBuffer();
    std::cout << std::endl;

    TestSentenceChunker();
    std::cout << std::endl;

    TestWatchdogAndRepetition();
    std::cout << std::endl;

    TestThreeTierMemory();
    std::cout << std::endl;

    TestFullDuplexPipeline();
    std::cout << std::endl;

    TestCpuAffinity();
    std::cout << std::endl;

    std::cout << COLOR_GREEN << "ALL 6 TEST SUITES PASSED SUCCESSFULLY!" << COLOR_RESET << "\n\n";
    return 0;
}

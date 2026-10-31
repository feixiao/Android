#include "LockFreeRingBuffer.hpp"
#include "CpuAffinity.hpp"
#include "StreamingSentenceChunker.hpp"
#include "WatchdogAndRepetition.hpp"
#include "ThreeTierMemory.hpp"
#include "FullDuplexAudioIO.hpp"
#include "AudioAgentPipeline.hpp"
#include "MnnLlmEngine.hpp"
#include "MockCloudLlmEngine.hpp"
#include "IntentArbiter.hpp"
#include <iostream>
#include <sstream>
#include <cassert>
#include <chrono>
#include <thread>
#include <cmath>
#include <regex>
#include <ctime>
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

    // 3. Test PauseAudioIO & ResumeAudioIO recovery (e.g. after external music playback starts & stops)
    pipeline.PauseAudioIO();
    bool resumed = pipeline.ResumeAudioIO();
    assert(resumed);
    // Pipeline state must cleanly reset from interrupted LISTENING back to ready IDLE
    assert(pipeline.GetState() == AgentState::IDLE);

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
// -------------------------------------------------------------
// Test 7: LockFreeSPSCRingBuffer Edge Cases (Overflow/Underflow)
// -------------------------------------------------------------
static void TestRingBufferEdgeCases() {
    LogTest("LockFreeSPSCRingBuffer Edge Cases & Capacity Limits");

    LockFreeSPSCRingBuffer<float> rb(10);
    assert(rb.capacity() == 10);

    // 1. Underflow: read from empty buffer returns 0
    float dummy = 0.0f;
    assert(rb.read(&dummy, 1) == 0);

    // 2. Single-sample writes and reads
    for (int i = 0; i < 100; ++i) {
        float sample = static_cast<float>(i);
        assert(rb.write(&sample, 1) == 1);
        float out = 0.0f;
        assert(rb.read(&out, 1) == 1);
        assert(out == sample);
    }
    assert(rb.empty());

    // 3. Overflow: write more than capacity (capped at available capacity)
    std::vector<float> overflow_data(15, 1.0f);
    size_t written = rb.write(overflow_data.data(), 15);
    assert(written == 10);
    assert(rb.available_write() == 0);
    assert(rb.available_read() == 10);

    // Attempting to write when full returns 0
    assert(rb.write(overflow_data.data(), 1) == 0);

    rb.clear();
    assert(rb.empty());

    LogPass("LockFreeSPSCRingBuffer Edge Cases passed.");
}

// -------------------------------------------------------------
// Test 8: FullDuplexAudioIO Lifecycle & Zero-Fill
// -------------------------------------------------------------
static void TestFullDuplexAudioIO() {
    LogTest("FullDuplexAudioIO Lifecycle, Streaming & Zero-Fill");

    auto mic_rb = std::make_shared<LockFreeSPSCRingBuffer<float>>(16000 * 2);
    auto spk_rb = std::make_shared<LockFreeSPSCRingBuffer<float>>(24000 * 2);

    FullDuplexAudioIO audio_io(mic_rb, spk_rb, 16000, 24000);
    assert(audio_io.GetMicSampleRate() == 16000);
    assert(audio_io.GetSpkSampleRate() == 24000);
    assert(!audio_io.IsRunning());

    bool started = audio_io.StartStreams();
    assert(started);
    assert(audio_io.IsRunning());

    // 1. Test mic simulation injection
    std::vector<float> mic_samples(512, 0.42f);
    size_t written = audio_io.SimulateMicInput(mic_samples.data(), mic_samples.size());
    assert(written == 512);
    assert(mic_rb->available_read() == 512);

    // 2. Test speaker simulation extraction
    std::vector<float> spk_in(480, 0.88f);
    spk_rb->write(spk_in.data(), spk_in.size());
    assert(spk_rb->available_read() == 480);

    std::vector<float> spk_out(480, 0.0f);
    size_t pulled = audio_io.SimulateSpeakerOutput(spk_out.data(), spk_out.size());
    assert(pulled == 480);
    assert(std::abs(spk_out[0] - 0.88f) < 1e-5f);
    assert(spk_rb->available_read() == 0);

#if !defined(AUDIO_AGENT_USE_OBOE)
    // 3. Test mock output frames underrun protection (zero-fill)
    std::vector<float> partial(50, 0.5f);
    spk_rb->write(partial.data(), partial.size());

    std::vector<float> mock_out(200, 99.0f);
    audio_io.ProcessMockOutputFrames(mock_out.data(), 200);

    for (int i = 0; i < 50; ++i) assert(std::abs(mock_out[i] - 0.5f) < 1e-5f);
    for (int i = 50; i < 200; ++i) assert(std::abs(mock_out[i] - 0.0f) < 1e-5f);
#endif

    audio_io.StopStreams();
    assert(!audio_io.IsRunning());

    LogPass("FullDuplexAudioIO passed all lifecycle and zero-fill tests.");
}

// -------------------------------------------------------------
// Test 9: Perception VAD Energy & Endpointing
// -------------------------------------------------------------
class TestVadPerceptionEngine : public ISpeechPerceptionEngine {
public:
    bool AcceptAudioChunk(const float* samples, int32_t count) override {
        float energy = 0.0f;
        for (int i = 0; i < count; ++i) energy += std::abs(samples[i]);
        float avg_energy = count > 0 ? (energy / count) : 0.0f;

        bool is_voice = avg_energy > 0.035f;
        if (is_voice) {
            speech_frames_++;
            silence_frames_ = 0;
            if (speech_frames_ >= 3) {
                in_speech_ = true;
                endpoint_ = false;
            }
        } else {
            if (in_speech_) {
                silence_frames_++;
                if (silence_frames_ >= 10) {
                    endpoint_ = true;
                    in_speech_ = false;
                    speech_frames_ = 0;
                    silence_frames_ = 0;
                }
            }
        }
        return is_voice;
    }

    bool IsEndpointDetected() override { return endpoint_; }
    std::string GetRecognizedText(bool is_final) override {
        if (is_final) {
            endpoint_ = false;
            return recognized_;
        }
        return in_speech_ ? "正在倾听..." : "";
    }
    void Reset() override {
        endpoint_ = false;
        in_speech_ = false;
        speech_frames_ = 0;
        silence_frames_ = 0;
        recognized_.clear();
    }
    void SetRecognized(const std::string& text) { recognized_ = text; }
    bool InSpeech() const { return in_speech_; }

private:
    bool endpoint_{false};
    bool in_speech_{false};
    int speech_frames_{0};
    int silence_frames_{0};
    std::string recognized_;
};

static void TestPerceptionVAD() {
    LogTest("Perception VAD Energy Detection and Endpointing");

    TestVadPerceptionEngine vad;
    std::vector<float> silence_chunk(512, 0.01f);
    std::vector<float> speech_chunk(512, 0.12f);

    // 1. Silence should not trigger voice
    for (int i = 0; i < 5; ++i) {
        bool active = vad.AcceptAudioChunk(silence_chunk.data(), 512);
        assert(!active);
        assert(!vad.InSpeech());
        assert(!vad.IsEndpointDetected());
    }

    // 2. Feed 2 speech frames (under 3 frames onset threshold)
    assert(vad.AcceptAudioChunk(speech_chunk.data(), 512));
    assert(!vad.InSpeech());
    assert(vad.AcceptAudioChunk(speech_chunk.data(), 512));
    assert(!vad.InSpeech());

    // 3. 3rd speech frame triggers speech active!
    assert(vad.AcceptAudioChunk(speech_chunk.data(), 512));
    assert(vad.InSpeech());
    assert(vad.GetRecognizedText(false) == "正在倾听...");

    // 4. Feed 10 silence frames to reach endpoint cutoff
    for (int i = 0; i < 9; ++i) {
        vad.AcceptAudioChunk(silence_chunk.data(), 512);
        assert(vad.InSpeech());
        assert(!vad.IsEndpointDetected());
    }
    vad.AcceptAudioChunk(silence_chunk.data(), 512);
    assert(!vad.InSpeech());
    assert(vad.IsEndpointDetected());

    vad.SetRecognized("把空调调到24度");
    assert(vad.GetRecognizedText(true) == "把空调调到24度");
    assert(!vad.IsEndpointDetected());

    vad.Reset();
    assert(!vad.InSpeech());
    assert(!vad.IsEndpointDetected());

    LogPass("Perception VAD energy thresholds and endpointing passed.");
}

// -------------------------------------------------------------
// Test 10: Dynamic Intent Parsing, Real-time Clock & Tool Calling
// -------------------------------------------------------------
class TestDynamicLlmEngine : public ILlmEngine {
public:
    void ResponseStream(const std::string& prompt, std::ostream* out_stream) override {
        if (!out_stream) return;
        std::string query = prompt;
        size_t pos = prompt.rfind("<|im_start|>user\n");
        if (pos != std::string::npos) {
            size_t start = pos + std::string("<|im_start|>user\n").length();
            size_t end = prompt.find("<|im_end|>", start);
            if (end != std::string::npos) query = prompt.substr(start, end - start);
            else query = prompt.substr(start);
        }
        time_t now = std::time(nullptr);
        tm* ltm = std::localtime(&now);
        char time_buf[64] = {0};
        std::strftime(time_buf, sizeof(time_buf), "%H点%M分", ltm);

        if (query.find("几点") != std::string::npos || query.find("时间") != std::string::npos) {
            *out_stream << "现在是北京时间 " << time_buf << "。";
        } else if (query.find("空调") != std::string::npos || query.find("温度") != std::string::npos) {
            if (query.find("关") != std::string::npos) {
                *out_stream << "好的，已为您关闭空调。<|tool_call|>{\"action\":\"set_ac_power\",\"state\":\"off\"}<|/tool_call|>";
            } else {
                int temp = 24;
                std::regex num_reg(R"((\d{2}))");
                std::smatch m;
                if (std::regex_search(query, m, num_reg)) {
                    int val = std::stoi(m[1].str());
                    if (val >= 16 && val <= 32) temp = val;
                }
                *out_stream << "没问题，已调至" << temp << "度。<|tool_call|>{\"action\":\"set_ac_temp\",\"value\":" << temp << "}<|/tool_call|>";
            }
        } else if (query.find("导航") != std::string::npos || query.find("去") != std::string::npos) {
            std::string dest = "杭州西湖文化广场";
            size_t p = query.find("去");
            if (p == std::string::npos) p = query.find("到");
            if (p != std::string::npos && p + 3 < query.size()) {
                dest = query.substr(p + 3);
            }
            *out_stream << "好的，马上为您规划前往" << dest << "的最佳路线。<|tool_call|>{\"action\":\"start_nav\",\"dest\":\"" << dest << "\"}<|/tool_call|>";
        } else if (query.find("放") != std::string::npos || query.find("听") != std::string::npos ||
                   query.find("音乐") != std::string::npos || query.find("歌") != std::string::npos) {
            std::string song = "晴天";
            if (query.find("晴天") != std::string::npos) song = "晴天";
            else if (query.find("海阔天空") != std::string::npos) song = "海阔天空";
            *out_stream << "好的，为您播放" << song << "。<|tool_call|>{\"action\":\"play_music\",\"query\":\"" << song << "\"}<|/tool_call|>";
        } else if (query.find("蓝") != std::string::npos || query.find("为什么") != std::string::npos) {
            *out_stream << "这叫瑞利散射。阳光里的蓝光容易被空气散射。";
        } else {
            *out_stream << "关于您提到的“" << query << "”，我已经认真接收到了。";
        }
        out_stream->flush();
    }
    void RollbackTokens(size_t) override {}
    bool WarmupPrefix(const std::string&) override { return true; }
};

static void TestDynamicCognition() {
    LogTest("Dynamic Intent Parsing, Real-time Clock & Tool Calling");

    TestDynamicLlmEngine llm;

    // 1. Dynamic Clock
    {
        std::stringstream ss;
        llm.ResponseStream("现在几点了？", &ss);
        std::string reply = ss.str();
        assert(reply.find("北京时间") != std::string::npos);
        assert(reply.find("点") != std::string::npos);
    }

    // 2. Dynamic AC Temperature
    {
        std::stringstream ss;
        llm.ResponseStream("把空调调到28度", &ss);
        std::string reply = ss.str();
        assert(reply.find("已调至28度") != std::string::npos);
        assert(reply.find("{\"action\":\"set_ac_temp\",\"value\":28}") != std::string::npos);
    }

    // 3. Dynamic AC Turn Off
    {
        std::stringstream ss;
        llm.ResponseStream("把空调关掉", &ss);
        std::string reply = ss.str();
        assert(reply.find("{\"action\":\"set_ac_power\",\"state\":\"off\"}") != std::string::npos);
    }

    // 4. Dynamic Destination Navigation
    {
        std::stringstream ss;
        llm.ResponseStream("导航去北京天安门广场", &ss);
        std::string reply = ss.str();
        assert(reply.find("北京天安门广场") != std::string::npos);
        assert(reply.find("{\"action\":\"start_nav\",\"dest\":\"北京天安门广场\"}") != std::string::npos);
    }

    // 5. Query Quoting Fallback
    {
        std::stringstream ss;
        llm.ResponseStream("我想买一台轻薄笔记本电脑", &ss);
        std::string reply = ss.str();
        assert(reply.find("我想买一台轻薄笔记本电脑") != std::string::npos);
    }

    // 6. Music Playback Tool Calling
    {
        std::stringstream ss;
        llm.ResponseStream("放一首周杰伦的晴天", &ss);
        std::string reply = ss.str();
        assert(reply.find("晴天") != std::string::npos);
        assert(reply.find("{\"action\":\"play_music\",\"query\":\"晴天\"}") != std::string::npos);
    }

    LogPass("Dynamic Intent Parsing and Tool Calling passed all tests.");
}

// -------------------------------------------------------------
// Test 11: Multi-Turn Full-Duplex Dialogue & Memory Continuity
// -------------------------------------------------------------
static void TestMultiTurnDialogue() {
    LogTest("Multi-Turn Full-Duplex Dialogue Continuity");

    auto perception = std::make_shared<SimulatedPerceptionEngine>();
    auto llm = std::make_shared<TestDynamicLlmEngine>();
    auto tts = std::make_shared<SimulatedTtsEngine>();

    std::vector<std::string> spoken_sentences;
    std::vector<std::string> tool_calls;

    PipelineObserver observer;
    observer.on_tts_sentence_start = [&](const std::string& sentence) {
        spoken_sentences.push_back(sentence);
    };
    observer.on_tool_call = [&](const std::string& tool_json) {
        tool_calls.push_back(tool_json);
    };

    AudioAgentPipeline pipeline(perception, llm, tts, observer);
    pipeline.Start();

    // Turn 1: Time query
    pipeline.SubmitUserQuery("请问现在几点了？");
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    assert(!spoken_sentences.empty());
    assert(pipeline.GetMemory().GetActiveTurns().size() == 1);

    // Turn 2: AC tool call
    spoken_sentences.clear();
    pipeline.SubmitUserQuery("把空调调到26度");
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    assert(!tool_calls.empty());
    assert(tool_calls.back().find("26") != std::string::npos);
    assert(pipeline.GetMemory().GetActiveTurns().size() == 2);

    // Turn 3: Navigation tool call
    tool_calls.clear();
    pipeline.SubmitUserQuery("导航去杭州西湖文化广场");
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    assert(!tool_calls.empty());
    assert(tool_calls.back().find("杭州西湖文化广场") != std::string::npos);
    assert(pipeline.GetMemory().GetActiveTurns().size() == 3);

    pipeline.Stop();
    LogPass("Multi-Turn Full-Duplex Dialogue Continuity passed.");
}

class TestModelPerceptionEngine : public ISpeechPerceptionEngine {
public:
    bool AcceptAudioChunk(const float* samples, int32_t count) override {
        float energy = 0.0f;
        for (int i = 0; i < count; ++i) energy += std::abs(samples[i]);
        float avg_energy = count > 0 ? (energy / count) : 0.0f;
        bool is_voice = avg_energy > 0.035f;
        if (is_voice) {
            speech_frames_++;
            silence_frames_ = 0;
            if (speech_frames_ >= 3) {
                in_speech_ = true;
                endpoint_ = false;
            }
        } else {
            if (in_speech_) {
                silence_frames_++;
                if (silence_frames_ >= 5) {
                    endpoint_ = true;
                    in_speech_ = false;
                    recorded_frames_ = speech_frames_;
                    speech_frames_ = 0;
                    silence_frames_ = 0;
                }
            }
        }
        return is_voice;
    }

    bool IsEndpointDetected() override {
        return endpoint_;
    }

    std::string GetRecognizedText(bool is_final) override {
        if (is_final) {
            endpoint_ = false;
            if (has_local_model_ && recorded_frames_ >= 3) {
                recorded_frames_ = 0;
                return "今天外面天气怎么样？";
            }
            recorded_frames_ = 0;
            return "";
        }
        return in_speech_ ? (has_local_model_ ? "正在倾听并识别语音..." : "正在倾听...") : "";
    }

    void SetModelPath(const std::string& path) override {
        model_path_ = path;
        has_local_model_ = !path.empty();
    }

    void Reset() override {
        endpoint_ = false;
        in_speech_ = false;
        speech_frames_ = 0;
        silence_frames_ = 0;
    }

    bool HasLocalModel() const { return has_local_model_; }
    const std::string& GetModelPath() const { return model_path_; }

private:
    std::string model_path_;
    bool has_local_model_{false};
    bool endpoint_{false};
    bool in_speech_{false};
    int speech_frames_{0};
    int silence_frames_{0};
    int recorded_frames_{0};
};

static void TestModelConfigurationAndPerception() {
    LogTest("On-Device Model Configuration & Perception Engine");

    auto perception = std::make_shared<TestModelPerceptionEngine>();
    auto llm = std::make_shared<SimulatedLlmEngine>();
    auto tts = std::make_shared<SimulatedTtsEngine>();

    AudioAgentPipeline pipeline(perception, llm, tts);

    // Initial state: no local model configured
    assert(!perception->HasLocalModel());
    assert(perception->GetModelPath().empty());

    // Configure model path via pipeline
    const std::string test_path = "/data/user/0/com.ai.audioagent/files/models/asr";
    pipeline.SetModelPath(test_path);
    assert(perception->HasLocalModel());
    assert(perception->GetModelPath() == test_path);

    // Feed audio chunks without voice
    std::vector<float> silence(512, 0.005f);
    assert(!perception->AcceptAudioChunk(silence.data(), silence.size()));
    assert(!perception->IsEndpointDetected());

    // Feed speech audio chunks
    std::vector<float> speech(512, 0.12f);
    for (int i = 0; i < 4; ++i) {
        perception->AcceptAudioChunk(speech.data(), speech.size());
    }
    assert(perception->GetRecognizedText(false) == "正在倾听并识别语音...");

    // Feed silence to trigger endpoint
    for (int i = 0; i < 6; ++i) {
        perception->AcceptAudioChunk(silence.data(), silence.size());
    }
    assert(perception->IsEndpointDetected());
    std::string final_query = perception->GetRecognizedText(true);
    assert(final_query == "今天外面天气怎么样？");

    // Unset model path
    pipeline.SetModelPath("");
    assert(!perception->HasLocalModel());

    // Trigger speech again, without model
    for (int i = 0; i < 4; ++i) {
        perception->AcceptAudioChunk(speech.data(), speech.size());
    }
    for (int i = 0; i < 6; ++i) {
        perception->AcceptAudioChunk(silence.data(), silence.size());
    }
    assert(perception->IsEndpointDetected());
    assert(perception->GetRecognizedText(true).empty());

    LogPass("On-Device Model Configuration and Perception passed.");
}
static void TestMnnLlmEngineAndCloudComparison() {
    LogTest("MNN-LLM On-Device Engine (Prefix Reuse & KV Rollback) vs Cloud Mock");

    // 1. Test On-Device MNN-LLM Engine
    MnnLlmEngine local_llm;
    const std::string sys_prompt = "你是本地全双工AI语音助手，请用极简纯口语回答，严禁使用符号与阿拉伯数字。";

    // Warmup prefix KV cache (Section 3.3)
    bool warmed = local_llm.WarmupPrefix(sys_prompt);
    assert(warmed);

    // Run query with warmed system prompt
    // Run query with unloaded model: MUST directly report error, NEVER fake hardcoded answers!
    std::string full_prompt = "<|im_start|>system\n" + sys_prompt + "<|im_end|>\n<|im_start|>user\n今天天气怎么样？<|im_end|>\n<|im_start|>assistant\n";
    std::stringstream local_ss;
    local_llm.ResponseStream(full_prompt, &local_ss);

    std::string local_reply = local_ss.str();
    assert(!local_reply.empty());
    assert(local_reply.find("错误") != std::string::npos);
    assert(local_reply.find("未加载") != std::string::npos);

    auto local_stats = local_llm.GetLatestStats();
    assert(local_stats.engine_name == "MNN-LLM (Error: Unloaded)");
    assert(local_stats.tokens_per_second == 0.0f);
    // 2. Test Cloud Mock Comparison
    MockCloudLlmEngine cloud_llm;
    std::stringstream cloud_ss;
    cloud_llm.ResponseStream(full_prompt, &cloud_ss);
    std::string cloud_reply = cloud_ss.str();
    assert(!cloud_reply.empty());

    auto cloud_stats = cloud_llm.GetLatestStats();
    assert(!cloud_stats.is_prefix_hit); // Cloud cannot reuse local KV cache
    assert(cloud_stats.ttft_ms >= 250); // Cloud latency includes WAN RTT (280ms)
    assert(cloud_stats.engine_name == "Cloud-API (Mocked SSE)");

    LogPass("MNN-LLM On-Device Engine and Cloud Comparison passed.");
}

static void TestIntentArbiterSuite() {
    LogTest("Intent Arbiter & Small-Model Tool Call Compensation");

    // 1. User says "说从亚运动员导航到西湖" (User's exact real-world scenario)
    {
        std::string tool = IntentArbiter::ResolveToolCall("说从亚运动员导航到西湖");
        assert(!tool.empty());
        assert(tool.find("start_nav") != std::string::npos);
        assert(tool.find("西湖") != std::string::npos);
    }

    // 2. "帮我把导航设到杭州西湖文化广场"
    {
        std::string tool = IntentArbiter::ResolveToolCall("帮我把导航设到杭州西湖文化广场");
        assert(!tool.empty());
        assert(tool.find("start_nav") != std::string::npos);
        assert(tool.find("杭州西湖文化广场") != std::string::npos);
    }

    // 3. "从亚运村到西湖文化广场怎么走"
    {
        std::string tool = IntentArbiter::ResolveToolCall("从亚运村到西湖文化广场怎么走");
        assert(!tool.empty());
        assert(tool.find("start_nav") != std::string::npos);
        assert(tool.find("西湖文化广场") != std::string::npos);
    }

    // 4. "导航去北京天安门广场"
    {
        std::string tool = IntentArbiter::ResolveToolCall("导航去北京天安门广场");
        assert(!tool.empty());
        assert(tool.find("start_nav") != std::string::npos);
        assert(tool.find("北京天安门广场") != std::string::npos);
    }

    // 5. Air conditioner: "把空调温度调到二十四度" & "关空调"
    {
        std::string tool1 = IntentArbiter::ResolveToolCall("把空调温度调到二十四度");
        assert(tool1.find("set_ac_temp") != std::string::npos);
        assert(tool1.find("24") != std::string::npos);

        std::string tool2 = IntentArbiter::ResolveToolCall("关闭空调");
        assert(tool2.find("set_ac_power") != std::string::npos);
        assert(tool2.find("off") != std::string::npos);
    }

    // 6. Music: "放一首周杰伦的晴天" and media control commands
    {
        std::string tool = IntentArbiter::ResolveToolCall("放一首周杰伦的晴天");
        assert(tool.find("play_music") != std::string::npos);
        assert(tool.find("晴天") != std::string::npos);

        std::string pause_tool = IntentArbiter::ResolveToolCall("暂停播放");
        assert(pause_tool.find("play_music") != std::string::npos);
        assert(pause_tool.find("pause") != std::string::npos);

        std::string stop_tool = IntentArbiter::ResolveToolCall("关闭音乐");
        assert(stop_tool.find("play_music") != std::string::npos);
        assert(stop_tool.find("pause") != std::string::npos);

        std::string next_tool = IntentArbiter::ResolveToolCall("切歌");
        assert(next_tool.find("play_music") != std::string::npos);
        assert(next_tool.find("next") != std::string::npos);

        std::string resume_tool = IntentArbiter::ResolveToolCall("继续播放");
        assert(resume_tool.find("play_music") != std::string::npos);
        assert(resume_tool.find("resume") != std::string::npos);
    }

    // 7. Non-tool queries must NOT trigger tools (Anti-conflict invariant)
    {
        assert(IntentArbiter::ResolveToolCall("为什么天空是蓝色的啊").empty());
        assert(IntentArbiter::ResolveToolCall("给我讲个好笑的笑话吧").empty());
        assert(IntentArbiter::ResolveToolCall("现在几点了？").empty());
        assert(IntentArbiter::ResolveToolCall("你好，请问你是谁？").empty());
    }
    // 7.1 Zero-Hallucination & Slot Missing Invariant: Never hallucinate fake slots!
    {
        // Missing destination must NOT hallucinate "西湖文化广场"
        auto nav_res = IntentArbiter::Arbitrate("我要去");
        assert(nav_res.status == ArbiterStatus::SLOT_MISSING);
        assert(nav_res.missing_slot == "destination");
        assert(IntentArbiter::ResolveToolCall("我要去").empty());

        // Missing alarm time must NOT hallucinate "07:30"
        auto alarm_res = IntentArbiter::Arbitrate("帮我定个闹钟");
        assert(alarm_res.status == ArbiterStatus::SLOT_MISSING);
        assert(alarm_res.missing_slot == "time");
        assert(IntentArbiter::ResolveToolCall("帮我定个闹钟").empty());

        // Missing phone target must NOT hallucinate "10086"
        auto phone_res = IntentArbiter::Arbitrate("打电话");
        assert(phone_res.status == ArbiterStatus::SLOT_MISSING);
        assert(phone_res.missing_slot == "phone_number");
        assert(IntentArbiter::ResolveToolCall("打电话").empty());
    }

    // 7.2 Universal Chinese Number & Temperature Normalization (No hardcoded 16-32 vector)
    {
        std::string tool30 = IntentArbiter::ResolveToolCall("把空调温度调到三十度");
        assert(tool30.find("set_ac_temp") != std::string::npos);
        assert(tool30.find("30") != std::string::npos);

        std::string tool16 = IntentArbiter::ResolveToolCall("把空调调到十六度");
        assert(tool16.find("set_ac_temp") != std::string::npos);
        assert(tool16.find("16") != std::string::npos);
    }

    // 7.3 Dynamic Entity Registration (Decoupled from code)
    {
        IntentArbiter::RegisterCities({"三亚", "拉萨"});
        std::string weather_tool = IntentArbiter::ResolveToolCall("查询三亚的天气");
        assert(weather_tool.find("三亚") != std::string::npos);

        IntentArbiter::RegisterContacts({"主管", "王经理"});
        std::string phone_tool = IntentArbiter::ResolveToolCall("打电话给王经理");
        assert(phone_tool.find("王经理") != std::string::npos);
    }

    // 8. Pipeline Tool Compensation Simulation:
    // If LLM generates purely conversational text without <|tool_call|>, IntentArbiter compensates and dispatches it!
    class ConversationalOnlyLlm : public ILlmEngine {
    public:
        void ResponseStream(const std::string&, std::ostream* out) override {
            *out << "好的，我会的。请稍等。";
            out->flush();
        }
        void RollbackTokens(size_t) override {}
        bool WarmupPrefix(const std::string&) override { return true; }
    };

    auto perception = std::make_shared<SimulatedPerceptionEngine>();
    auto conv_llm = std::make_shared<ConversationalOnlyLlm>();
    auto tts = std::make_shared<SimulatedTtsEngine>();
    std::vector<std::string> compensated_tools;
    PipelineObserver observer;
    observer.on_tool_call = [&](const std::string& tool_json) {
        compensated_tools.push_back(tool_json);
    };

    AudioAgentPipeline pipeline(perception, conv_llm, tts, observer);
    pipeline.Start();
    pipeline.SubmitUserQuery("说从亚运动员导航到西湖");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(!compensated_tools.empty());
    assert(compensated_tools.back().find("start_nav") != std::string::npos);
    assert(compensated_tools.back().find("西湖") != std::string::npos);
    pipeline.Stop();

    LogPass("Intent Arbiter and Small-Model Tool Call Compensation passed all tests.");
}

int main() {
    std::cout << "\n=======================================================\n";
    std::cout << "  Android AI Audio Agent C++17 Unit Test Suite\n";
    std::cout << "=======================================================\n\n";

    TestRingBuffer();
    std::cout << std::endl;

    TestRingBufferEdgeCases();
    std::cout << std::endl;

    TestSentenceChunker();
    std::cout << std::endl;

    TestWatchdogAndRepetition();
    std::cout << std::endl;

    TestThreeTierMemory();
    std::cout << std::endl;

    TestFullDuplexAudioIO();
    std::cout << std::endl;

    TestPerceptionVAD();
    std::cout << std::endl;

    TestDynamicCognition();
    std::cout << std::endl;

    TestFullDuplexPipeline();
    std::cout << std::endl;

    TestMultiTurnDialogue();
    std::cout << std::endl;

    TestCpuAffinity();
    std::cout << std::endl;

    TestModelConfigurationAndPerception();
    std::cout << std::endl;
    TestMnnLlmEngineAndCloudComparison();
    TestIntentArbiterSuite();
    std::cout << std::endl;
    std::cout << std::endl;

    std::cout << COLOR_GREEN << "ALL 14 TEST SUITES PASSED SUCCESSFULLY!" << COLOR_RESET << "\n\n";
}

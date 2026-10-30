#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <chrono>
#include <functional>
#include <memory>

namespace audio_agent {

/**
 * Single turn in dialogue
 */
struct DialogueTurn {
    std::string user_query;
    std::string assistant_response;
    bool was_interrupted{false};
    int64_t timestamp_ms{0};
};

/**
 * Core summarized fact
 */
struct MemoryFact {
    std::string fact_text;
    std::vector<float> embedding; // Optional semantic embedding
    int64_t created_at_ms{0};
};

/**
 * Three-Tier Memory Subsystem:
 * - L1: Ring dialogue turns (Fixed system prompt + last 4 turns).
 * - L2: Idle-time semantic summarization (Triggered after 6 evicted turns + 10s idle).
 * - L3: Semantic fact store & retrieval for prompt augmentation.
 */
class ThreeTierMemory {
public:
    using SummarizeFunc = std::function<std::string(const std::vector<DialogueTurn>& turns)>;

    explicit ThreeTierMemory(std::string system_prompt =
        "你是运行于Android端侧的高性能实时语音助理。请用极简纯口语回答，严禁使用Markdown符号与阿拉伯数字。\n"
        "当用户需要导航、开闭空调调节温度、播放音乐、设置闹钟、查询天气、拨打电话时，必须在第一句给出即时口语安抚，并在紧随其后输出工具调用标签：<|tool_call|>{\"action\":\"<动作名>\",...}<|/tool_call|>。\n"
        "支持的工具动作：\n"
        "1. 导航：{\"action\":\"start_nav\",\"dest\":\"目的地\"}\n"
        "2. 空调：{\"action\":\"set_ac_temp\",\"value\":温度数字} 或 {\"action\":\"set_ac_power\",\"state\":\"on/off\"}\n"
        "3. 音乐：{\"action\":\"play_music\",\"query\":\"歌名或歌手\"}\n"
        "4. 闹钟：{\"action\":\"set_alarm\",\"time\":\"HH:mm\"}\n"
        "5. 天气：{\"action\":\"query_weather\",\"city\":\"城市名\"}\n"
        "6. 电话：{\"action\":\"dial_phone\",\"phone\":\"号码或姓名\"}"
    );
    ~ThreeTierMemory() = default;

    // Set custom background summarization function
    void SetSummarizer(SummarizeFunc summarizer) {
        std::lock_guard<std::mutex> lock(mtx_);
        summarizer_ = std::move(summarizer);
    }

    // L1: Record new user turn
    void AddTurn(const std::string& user_query, const std::string& assistant_response, bool was_interrupted = false);

    // L1: Rollback last assistant response on Barge-in (align with spoken text)
    void RollbackLastAssistantTurn(const std::string& actually_spoken_text);

    // Get current dialogue context for LLM prompt injection (System prompt + L3 facts + L1 turns)
    std::string FormatPromptContext(const std::string& current_query) const;

    // Check if idle summarization should run (called periodically by idle monitor)
    bool MaybeTriggerIdleSummarization(int64_t current_time_ms);

    // Directly insert an L3 fact
    void AddFact(const std::string& fact);

    // Retrieve relevant facts for a query (top_k)
    std::vector<std::string> RetrieveRelevantFacts(const std::string& query, size_t top_k = 2) const;

    // Get all stored facts
    std::vector<std::string> GetAllFacts() const;

    // Get L1 active dialogue history
    std::vector<DialogueTurn> GetActiveTurns() const;

    const std::string& GetSystemPrompt() const { return system_prompt_; }
    void SetSystemPrompt(std::string system_prompt) {
        std::lock_guard<std::mutex> lock(mtx_);
        system_prompt_ = std::move(system_prompt);
    }


private:
    std::string system_prompt_;
    mutable std::mutex mtx_;

    // L1: Max 4 active turns (8 sentences)
    static constexpr size_t MAX_L1_TURNS = 4;
    std::deque<DialogueTurn> l1_active_turns_;

    // L2: Accumulator of evicted turns awaiting summarization
    static constexpr size_t L2_EVICTED_TRIGGER_THRESHOLD = 6;
    static constexpr int64_t IDLE_TIME_TRIGGER_MS = 10000; // 10 seconds idle
    std::vector<DialogueTurn> evicted_turns_pending_summary_;
    int64_t last_activity_timestamp_ms_{0};
    SummarizeFunc summarizer_;

    // L3: Long-term core facts
    std::vector<MemoryFact> l3_facts_;
};

} // namespace audio_agent

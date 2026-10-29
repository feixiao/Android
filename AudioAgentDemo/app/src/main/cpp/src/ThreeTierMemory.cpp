#include "ThreeTierMemory.hpp"
#include <sstream>
#include <algorithm>
#include <chrono>

namespace audio_agent {

static int64_t GetCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

ThreeTierMemory::ThreeTierMemory(std::string system_prompt)
    : system_prompt_(std::move(system_prompt)),
      last_activity_timestamp_ms_(GetCurrentTimeMs()) {
    // Default fallback summarizer
    summarizer_ = [](const std::vector<DialogueTurn>& turns) -> std::string {
        if (turns.empty()) return "";
        std::ostringstream oss;
        oss << "用户历史对话偏好与事实（已归纳）：";
        for (const auto& t : turns) {
            oss << " [问: " << t.user_query << " 答: " << t.assistant_response << "]";
        }
        return oss.str();
    };
}

void ThreeTierMemory::AddTurn(const std::string& user_query, const std::string& assistant_response, bool was_interrupted) {
    std::lock_guard<std::mutex> lock(mtx_);
    int64_t now = GetCurrentTimeMs();
    last_activity_timestamp_ms_ = now;

    DialogueTurn turn;
    turn.user_query = user_query;
    turn.assistant_response = assistant_response;
    turn.was_interrupted = was_interrupted;
    turn.timestamp_ms = now;

    l1_active_turns_.push_back(turn);

    // L1 FIFO eviction when exceeding MAX_L1_TURNS (4 turns = 8 sentences)
    while (l1_active_turns_.size() > MAX_L1_TURNS) {
        evicted_turns_pending_summary_.push_back(l1_active_turns_.front());
        l1_active_turns_.pop_front();
    }
}

void ThreeTierMemory::RollbackLastAssistantTurn(const std::string& actually_spoken_text) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!l1_active_turns_.empty()) {
        auto& last = l1_active_turns_.back();
        last.assistant_response = actually_spoken_text + "（被用户打断）";
        last.was_interrupted = true;
    }
}

std::string ThreeTierMemory::FormatPromptContext(const std::string& current_query) const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::ostringstream oss;

    // 1. System Prompt (Bake-in or prefix)
    oss << "<|im_start|>system\n" << system_prompt_;

    // 2. L3 Memory Facts Injection (if any relevant facts exist)
    auto relevant_facts = RetrieveRelevantFacts(current_query, 2);
    if (!relevant_facts.empty()) {
        oss << "\n【用户长期记忆与习惯】:\n";
        for (const auto& fact : relevant_facts) {
            oss << "- " << fact << "\n";
        }
    }
    oss << "<|im_end|>\n";

    // 3. L1 Active Dialogue Turns
    for (const auto& turn : l1_active_turns_) {
        oss << "<|im_start|>user\n" << turn.user_query << "<|im_end|>\n";
        oss << "<|im_start|>assistant\n" << turn.assistant_response << "<|im_end|>\n";
    }

    // 4. Current User Query
    oss << "<|im_start|>user\n" << current_query << "<|im_end|>\n<|im_start|>assistant\n";
    return oss.str();
}

bool ThreeTierMemory::MaybeTriggerIdleSummarization(int64_t current_time_ms) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (evicted_turns_pending_summary_.size() < L2_EVICTED_TRIGGER_THRESHOLD) {
        return false;
    }

    if (current_time_ms - last_activity_timestamp_ms_ < IDLE_TIME_TRIGGER_MS) {
        return false; // Still within active conversation threshold
    }

    if (!summarizer_) {
        return false;
    }

    // Run L2 summarization
    std::string summary = summarizer_(evicted_turns_pending_summary_);
    evicted_turns_pending_summary_.clear();

    if (!summary.empty()) {
        MemoryFact fact;
        fact.fact_text = summary;
        fact.created_at_ms = current_time_ms;
        l3_facts_.push_back(std::move(fact));
        return true;
    }

    return false;
}

void ThreeTierMemory::AddFact(const std::string& fact) {
    std::lock_guard<std::mutex> lock(mtx_);
    MemoryFact mf;
    mf.fact_text = fact;
    mf.created_at_ms = GetCurrentTimeMs();
    l3_facts_.push_back(std::move(mf));
}

std::vector<std::string> ThreeTierMemory::RetrieveRelevantFacts(const std::string& query, size_t top_k) const {
    // Note: Caller already holds lock or this is called from locked methods
    std::vector<std::pair<int, std::string>> scored_facts;
    scored_facts.reserve(l3_facts_.size());

    for (const auto& fact : l3_facts_) {
        int score = 0;
        // Simple keyword match score across UTF-8 characters / tokens
        for (size_t i = 0; i + 3 <= query.size(); i += 3) {
            std::string sub = query.substr(i, 3);
            if (fact.fact_text.find(sub) != std::string::npos) {
                score++;
            }
        }
        scored_facts.emplace_back(score, fact.fact_text);
    }

    std::sort(scored_facts.begin(), scored_facts.end(), [](const auto& a, const auto& b) {
        return a.first > b.first;
    });

    std::vector<std::string> results;
    size_t count = std::min(top_k, scored_facts.size());
    for (size_t i = 0; i < count; ++i) {
        if (scored_facts[i].first > 0 || l3_facts_.size() <= top_k) {
            results.push_back(scored_facts[i].second);
        }
    }
    return results;
}

std::vector<std::string> ThreeTierMemory::GetAllFacts() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<std::string> facts;
    facts.reserve(l3_facts_.size());
    for (const auto& f : l3_facts_) {
        facts.push_back(f.fact_text);
    }
    return facts;
}

std::vector<DialogueTurn> ThreeTierMemory::GetActiveTurns() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return std::vector<DialogueTurn>(l1_active_turns_.begin(), l1_active_turns_.end());
}

} // namespace audio_agent

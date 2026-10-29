#include "ThreeTierMemory.hpp"
#include <sstream>
#include <algorithm>
#include <chrono>

namespace audio_agent {

/**
 * @brief 获取当前系统时间戳（毫秒）
 */
static int64_t GetCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

/**
 * @brief 构造函数：初始化系统 Prompt 与默认的对话归纳总结器
 */
ThreeTierMemory::ThreeTierMemory(std::string system_prompt)
    : system_prompt_(std::move(system_prompt)),
      last_activity_timestamp_ms_(GetCurrentTimeMs()) {
    // 默认兜底总结器：提取用户核心意图与回答要点
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

/**
 * @brief 记录一轮完整对话（用户问 + 助手答）至 L1 工作记忆
 * 
 * 机制：
 * - L1 保持固定滑动窗口（默认 MAX_L1_TURNS = 4 轮，即最近 8 句话）。
 * - 超过窗口时，最早的对话轮次被置换（Evict）移入待归纳队列 evicted_turns_pending_summary_，
 *   等待闲置时提炼为 L2/L3 长期事实。
 */
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

    // L1 FIFO 淘汰：超出最大轮数时移入待归纳缓冲区
    while (l1_active_turns_.size() > MAX_L1_TURNS) {
        evicted_turns_pending_summary_.push_back(l1_active_turns_.front());
        l1_active_turns_.pop_front();
    }
}

/**
 * @brief 打断回滚（Barge-in KV Rollback）
 * 
 * 关键设计：
 * 在全双工交互中，大模型可能一次性生成了 100 字，但播报到第 20 字时用户突然插话打断。
 * 若不回滚，下一次多轮对话 Prompt 会包含未曾播报的后 80 字，导致模型自以为用户已知悉后文，产生答非所问与逻辑错位。
 * 本方法将最近一轮 Assistant 回答精准截断为实际播报文本并标记打断后缀。
 */
void ThreeTierMemory::RollbackLastAssistantTurn(const std::string& actually_spoken_text) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!l1_active_turns_.empty()) {
        auto& last = l1_active_turns_.back();
        last.assistant_response = actually_spoken_text + "（被用户打断）";
        last.was_interrupted = true;
    }
}

/**
 * @brief 格式化组装符合 ChatML 规范的完整上下文字符串
 * 
 * 组装结构：
 * 1. <|im_start|>system\n {System Prompt} + {L3 检索出的历史画像/事实} <|im_end|>\n
 * 2. L1 近期多轮上下文对话（保留精确历史问答）
 * 3. <|im_start|>user\n {当前用户提问} <|im_end|>\n<|im_start|>assistant\n
 */
std::string ThreeTierMemory::FormatPromptContext(const std::string& current_query) const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::ostringstream oss;

    // 1. 系统提示词（System Prompt，支持前缀缓存 Prefix KV Cache）
    oss << "<|im_start|>system\n" << system_prompt_;

    // 2. 注入 L3 长期记忆事实（基于当前用户提问动态相关度检索）
    auto relevant_facts = RetrieveRelevantFacts(current_query, 2);
    if (!relevant_facts.empty()) {
        oss << "\n【用户长期记忆与习惯】:\n";
        for (const auto& fact : relevant_facts) {
            oss << "- " << fact << "\n";
        }
    }
    oss << "<|im_end|>\n";

    // 3. 注入 L1 活跃多轮历史
    for (const auto& turn : l1_active_turns_) {
        oss << "<|im_start|>user\n" << turn.user_query << "<|im_end|>\n";
        oss << "<|im_start|>assistant\n" << turn.assistant_response << "<|im_end|>\n";
    }

    // 4. 拼接当前提问与 Assistant 引导标记
    oss << "<|im_start|>user\n" << current_query << "<|im_end|>\n<|im_start|>assistant\n";
    return oss.str();
}

/**
 * @brief 对话间歇期闲置归纳触发器（运行于后台 IdleMonitor 线程）
 * 
 * 触发条件：
 * 1. 淘汰队列中的历史轮数达到阈值（L2_EVICTED_TRIGGER_THRESHOLD = 4 轮）；
 * 2. 距离上一次对话活跃时间已超过静默门限（IDLE_TIME_TRIGGER_MS = 15000ms，即 15 秒）；
 * 避免在用户密集对话时抢占 NPU/CPU 算力执行总结。
 */
bool ThreeTierMemory::MaybeTriggerIdleSummarization(int64_t current_time_ms) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (evicted_turns_pending_summary_.size() < L2_EVICTED_TRIGGER_THRESHOLD) {
        return false;
    }

    if (current_time_ms - last_activity_timestamp_ms_ < IDLE_TIME_TRIGGER_MS) {
        return false; // 用户可能仍在连续对话中，暂缓归纳
    }

    if (!summarizer_) {
        return false;
    }

    // 触发 L2 归纳计算：将淘汰的长对话浓缩成一句话事实
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

/**
 * @brief 手动持久化一条长期事实/用户画像偏好至 L3 记忆池
 */
void ThreeTierMemory::AddFact(const std::string& fact) {
    std::lock_guard<std::mutex> lock(mtx_);
    MemoryFact mf;
    mf.fact_text = fact;
    mf.created_at_ms = GetCurrentTimeMs();
    l3_facts_.push_back(std::move(mf));
}

/**
 * @brief 检索与当前提问最相关的 L3 长期事实
 * 
 * 原理：基于 UTF-8 字符/多音字词频重叠度（Token Match Score）快速初筛 Top-K 事实，
 * 避免引入沉重的向量检索库（Vector DB），保障端侧极低耗电与毫秒级匹配。
 */
std::vector<std::string> ThreeTierMemory::RetrieveRelevantFacts(const std::string& query, size_t top_k) const {
    std::vector<std::pair<int, std::string>> scored_facts;
    scored_facts.reserve(l3_facts_.size());

    for (const auto& fact : l3_facts_) {
        int score = 0;
        // 简易 UTF-8 三字节（中文字符）滑动窗口关键词命中打分
        for (size_t i = 0; i + 3 <= query.size(); i += 3) {
            std::string sub = query.substr(i, 3);
            if (fact.fact_text.find(sub) != std::string::npos) {
                score++;
            }
        }
        scored_facts.emplace_back(score, fact.fact_text);
    }

    // 按匹配分值降序排列
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

/**
 * @brief 获取所有已归纳的 L3 长期事实列表
 */
std::vector<std::string> ThreeTierMemory::GetAllFacts() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<std::string> facts;
    facts.reserve(l3_facts_.size());
    for (const auto& f : l3_facts_) {
        facts.push_back(f.fact_text);
    }
    return facts;
}

/**
 * @brief 获取当前 L1 工作记忆中的对话轮次列表
 */
std::vector<DialogueTurn> ThreeTierMemory::GetActiveTurns() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return std::vector<DialogueTurn>(l1_active_turns_.begin(), l1_active_turns_.end());
}

} // namespace audio_agent

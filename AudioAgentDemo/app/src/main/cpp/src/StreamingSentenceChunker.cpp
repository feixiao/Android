#include "StreamingSentenceChunker.hpp"
#include <vector>
#include <algorithm>

namespace audio_agent {

StreamingSentenceChunker::StreamingSentenceChunker(SentenceChunkerCallbacks callbacks)
    : callbacks_(std::move(callbacks)) {}

void StreamingSentenceChunker::Reset() {
    buffer_.clear();
    history_for_repetition_.clear();
    is_first_sentence_ = true;
    in_tool_call_ = false;
    has_dispatched_tool_ = false;
    sentences_emitted_ = 0;
    total_bytes_emitted_ = 0;
}

/**
 * @brief 核心流式拦截函数：重写 std::streambuf 的 xsputn
 * 
 * 当上游 LLM 引擎以流式（Streaming）形式吐出 token 时，通过重定向的 std::ostream 实时流入此函数。
 * 承担四大核心职责：
 * 1. 打断感知（Barge-in Abort）：检测用户是否已打断，若打断则返回 0 中止上游 LLM 生成。
 * 2. 幻觉死循环检测（N-Gram Repetition）：防范端侧小模型陷入重复词循环。
 * 3. 工具调用拦截（Tool Call Interception）：精准剥离 <|tool_call|> 协议标签，提取 JSON 并派发，避免 TTS 播报技术标签。
 * 4. 动态阈值断句（Dynamic Chunking）：首句极小阈值压降首字延迟（TTFT），后续句子增大阈值防范语音碎片化。
 */
std::streamsize StreamingSentenceChunker::xsputn(const char* s, std::streamsize n) {
    if (!s || n <= 0) return 0;

    // 1. 检查外部打断标志：若用户已插话，立即熔断上游大模型生成流
    if (callbacks_.is_aborted && callbacks_.is_aborted()) {
        return 0; // 返回 0 触发流写入失败，通知 LLM 提前终止
    }

    std::string token(s, static_cast<size_t>(n));
    total_bytes_emitted_ += token.size();

    // 2. 检查 N-gram 重复死循环退化
    if (DetectRepetition(token)) {
        if (callbacks_.on_repetition_detected) {
            callbacks_.on_repetition_detected();
        }
        return 0; // 陷入无限复读，强制中止生成
    }

    buffer_ += token;

    // 3. 工具调用协议检测与拦截：识别形如 <|tool_call|>{"name":"..."}<|/tool_call|>
    if (!in_tool_call_) {
        size_t open_pos = buffer_.find("<|tool_call|>");
        if (open_pos == std::string::npos) {
            open_pos = buffer_.find("<tool_call>");
        }
        if (open_pos != std::string::npos) {
            // 若标签前存在自然语音片段（如“好的，马上为您查询<|tool_call|>...”），优先将前缀送给 TTS 播报！
            if (open_pos > 0) {
                std::string prefix_text = buffer_.substr(0, open_pos);
                if (callbacks_.on_sentence_ready) {
                    callbacks_.on_sentence_ready(prefix_text);
                    sentences_emitted_++;
                }
                buffer_ = buffer_.substr(open_pos);
            }
            in_tool_call_ = true;
        }
    }

    if (in_tool_call_) {
        size_t close_pos = buffer_.find("<|/tool_call|>");
        size_t close_tag_len = 14;
        if (close_pos == std::string::npos) {
            close_pos = buffer_.find("</tool_call>");
            close_tag_len = 12;
        }
        if (close_pos != std::string::npos) {
            // 工具调用闭合标签完整到达，解析其内部 JSON 载荷
            std::string raw_tool_block = buffer_.substr(0, close_pos + close_tag_len);
            std::string json_body = ExtractToolJson(raw_tool_block);
            if (callbacks_.on_tool_call && !json_body.empty()) {
                callbacks_.on_tool_call(json_body);
                has_dispatched_tool_ = true;
            }
            // 从语音播报缓冲区中彻底抹除工具调用协议块，绝不送往 TTS 朗读
            buffer_.erase(0, close_pos + close_tag_len);
            in_tool_call_ = false;
        }
        return n;
    }

    // 4. 动态分块断句算法
    // - 首句（First Chunk）：门限为 FIRST_CHUNK_MIN_BYTES（15 字节，约 5 个汉字），遇标点即切分送入 TTS，实现首字极速发音；
    // - 后续句（Subsequent Chunks）：门限提高到 SUBSEQUENT_CHUNK_MIN_BYTES（40 字节，约 13 个汉字），避免断句过细导致听感碎裂。
    size_t min_chunk_bytes = is_first_sentence_ ? FIRST_CHUNK_MIN_BYTES : SUBSEQUENT_CHUNK_MIN_BYTES;
    if (buffer_.size() >= min_chunk_bytes && CheckPunctuation(token)) {
        if (callbacks_.on_sentence_ready) {
            callbacks_.on_sentence_ready(buffer_);
            sentences_emitted_++;
        }
        buffer_.clear();
        is_first_sentence_ = false;
    }

    return n;
}

/**
 * @brief 单字符溢出保底（重写 std::streambuf::overflow）
 */
int StreamingSentenceChunker::overflow(int c) {
    if (c != EOF) {
        char ch = static_cast<char>(c);
        xsputn(&ch, 1);
    }
    return c;
}

/**
 * @brief LLM 推理完毕时将缓冲区中残留的最后半句冲刷推送给 TTS
 */
void StreamingSentenceChunker::Flush() {
    if (callbacks_.is_aborted && callbacks_.is_aborted()) {
        buffer_.clear();
        return;
    }

    // 若流结束时仍处于工具调用状态，尝试容错提取并派发
    if (in_tool_call_) {
        std::string json_body = ExtractToolJson(buffer_);
        if (callbacks_.on_tool_call && !json_body.empty()) {
            callbacks_.on_tool_call(json_body);
            has_dispatched_tool_ = true;
        }
        buffer_.clear();
        in_tool_call_ = false;
        return;
    }

    // 若非工具调用状态且缓冲区残留文本，无条件发送尾句
    if (!buffer_.empty()) {
        if (callbacks_.on_sentence_ready) {
            callbacks_.on_sentence_ready(buffer_);
            sentences_emitted_++;
        }
        buffer_.clear();
    }
}

/**
 * @brief 校验新生成的 token 是否包含中英文自然停顿标点符号
 */
bool StreamingSentenceChunker::CheckPunctuation(const std::string& token) const {
    static const std::vector<std::string> puncts = {
        "，", "。", "！", "？", "；", "：", "、",
        ",", ".", "!", "?", ";", ":", "\n"
    };
    for (const auto& pt : puncts) {
        if (token.find(pt) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 滑动窗口检测文本生成是否陷入自循环退化
 * 
 * 机制：维护最近 256 字节的历史输出，如果尾部 REPETITION_TRIGGER_BYTES 长度的子串
 * 与紧挨着其前面的子串完全一致，则判定为发生复读退化。
 */
bool StreamingSentenceChunker::DetectRepetition(const std::string& new_text) {
    history_for_repetition_ += new_text;
    if (history_for_repetition_.size() > 256) {
        history_for_repetition_.erase(0, history_for_repetition_.size() - 256);
    }

    if (history_for_repetition_.size() >= REPETITION_TRIGGER_BYTES * 2) {
        size_t len = REPETITION_TRIGGER_BYTES;
        std::string suffix = history_for_repetition_.substr(history_for_repetition_.size() - len);
        std::string prior = history_for_repetition_.substr(history_for_repetition_.size() - len * 2, len);
        if (suffix == prior) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 提取 <|tool_call|> 与 <|/tool_call|> 中间的 JSON 结构体字符串
 */
std::string StreamingSentenceChunker::ExtractToolJson(const std::string& raw) {
    size_t start = raw.find("<|tool_call|>");
    size_t tag_len = 13;
    if (start == std::string::npos) {
        start = raw.find("<tool_call>");
        tag_len = 11;
    }

    if (start != std::string::npos) {
        start += tag_len;
        size_t end = raw.find("<|/tool_call|>", start);
        if (end == std::string::npos) {
            end = raw.find("</tool_call>", start);
        }
        if (end == std::string::npos) {
            return raw.substr(start);
        }
        return raw.substr(start, end - start);
    }

    // 容错：查找 raw 中的 JSON 结构体 {...}
    size_t brace_open = raw.find('{');
    size_t brace_close = raw.rfind('}');
    if (brace_open != std::string::npos && brace_close != std::string::npos && brace_close > brace_open) {
        std::string candidate = raw.substr(brace_open, brace_close - brace_open + 1);
        if (candidate.find("\"action\"") != std::string::npos) {
            return candidate;
        }
    }

    return "";
}

} // namespace audio_agent

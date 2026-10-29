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
    sentences_emitted_ = 0;
    total_bytes_emitted_ = 0;
}

std::streamsize StreamingSentenceChunker::xsputn(const char* s, std::streamsize n) {
    if (!s || n <= 0) return 0;

    // Check external barge-in abort flag
    if (callbacks_.is_aborted && callbacks_.is_aborted()) {
        return 0; // Terminate upstream LLM generation
    }

    std::string token(s, static_cast<size_t>(n));
    total_bytes_emitted_ += token.size();

    // Check for N-gram repetition loop
    if (DetectRepetition(token)) {
        if (callbacks_.on_repetition_detected) {
            callbacks_.on_repetition_detected();
        }
        return 0; // abort generation on infinite loop
    }

    buffer_ += token;

    // 1. Tool Call Detection: <|tool_call|> ... <|/tool_call|>
    if (!in_tool_call_) {
        size_t open_pos = buffer_.find("<|tool_call|>");
        if (open_pos != std::string::npos) {
            // If there was any speech text before the tool call tag, emit it first!
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
        if (close_pos != std::string::npos) {
            size_t tag_len = std::string("<|/tool_call|>").length();
            std::string raw_tool_block = buffer_.substr(0, close_pos + tag_len);
            std::string json_body = ExtractToolJson(raw_tool_block);
            if (callbacks_.on_tool_call && !json_body.empty()) {
                callbacks_.on_tool_call(json_body);
            }
            // Remove the tool call block from speech buffer
            buffer_.erase(0, close_pos + tag_len);
            in_tool_call_ = false;
        }
        return n;
    }

    // 2. Dynamic threshold sentence chunking
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

int StreamingSentenceChunker::overflow(int c) {
    if (c != EOF) {
        char ch = static_cast<char>(c);
        xsputn(&ch, 1);
    }
    return c;
}

void StreamingSentenceChunker::Flush() {
    if (callbacks_.is_aborted && callbacks_.is_aborted()) {
        buffer_.clear();
        return;
    }

    // If there is any un-emitted text outside a tool call, send it now
    if (!buffer_.empty() && !in_tool_call_) {
        if (callbacks_.on_sentence_ready) {
            callbacks_.on_sentence_ready(buffer_);
            sentences_emitted_++;
        }
        buffer_.clear();
    }
}

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

bool StreamingSentenceChunker::DetectRepetition(const std::string& new_text) {
    history_for_repetition_ += new_text;
    if (history_for_repetition_.size() > 256) {
        // Keep a rolling window of recent generation
        history_for_repetition_.erase(0, history_for_repetition_.size() - 256);
    }

    if (history_for_repetition_.size() >= REPETITION_TRIGGER_BYTES * 2) {
        // Check if the trailing REPETITION_TRIGGER_BYTES repeat the preceding sequence
        size_t len = REPETITION_TRIGGER_BYTES;
        std::string suffix = history_for_repetition_.substr(history_for_repetition_.size() - len);
        std::string prior = history_for_repetition_.substr(history_for_repetition_.size() - len * 2, len);
        if (suffix == prior) {
            return true;
        }
    }
    return false;
}

std::string StreamingSentenceChunker::ExtractToolJson(const std::string& raw) {
    const std::string open_tag = "<|tool_call|>";
    const std::string close_tag = "<|/tool_call|>";

    size_t start = raw.find(open_tag);
    if (start == std::string::npos) return "";
    start += open_tag.length();

    size_t end = raw.find(close_tag, start);
    if (end == std::string::npos) {
        return raw.substr(start);
    }
    return raw.substr(start, end - start);
}

} // namespace audio_agent

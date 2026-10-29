#pragma once

#include <sstream>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <cstdint>

namespace audio_agent {

/**
 * Callbacks invoked during streaming sentence chunking
 */
struct SentenceChunkerCallbacks {
    std::function<void(const std::string& sentence)> on_sentence_ready;
    std::function<void(const std::string& tool_json)> on_tool_call;
    std::function<bool()> is_aborted; // check abort flag
    std::function<void()> on_repetition_detected;
};

/**
 * Custom std::stringbuf that intercepts streaming tokens from LLM inference.
 * Implements:
 * 1. Dynamic sentence chunking: 6 bytes for the 1st sentence, 21 bytes for subsequent sentences.
 * 2. Punctuation detection across UTF-8 multi-byte boundaries.
 * 3. Silent interception and asynchronous dispatch of <|tool_call|> ... <|/tool_call|> blocks.
 * 4. N-gram repetition loop detection to abort model hallucinations.
 */
class StreamingSentenceChunker : public std::stringbuf {
public:
    explicit StreamingSentenceChunker(SentenceChunkerCallbacks callbacks);
    ~StreamingSentenceChunker() override = default;

    // Reset state for a new inference cycle
    void Reset();

    // Flush any pending non-empty content at end of LLM generation
    void Flush();

    // Extract tool JSON payload from <|tool_call|>...<|/tool_call|>
    static std::string ExtractToolJson(const std::string& raw);

    // Number of sentences generated in this cycle
    size_t GetSentencesEmittedCount() const { return sentences_emitted_; }

    // Total characters/bytes emitted
    size_t GetTotalBytesEmitted() const { return total_bytes_emitted_; }

protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override;
    int overflow(int c) override;

private:
    bool CheckPunctuation(const std::string& token) const;
    bool DetectRepetition(const std::string& new_text);

    SentenceChunkerCallbacks callbacks_;
    std::string buffer_;
    std::string history_for_repetition_;
    bool is_first_sentence_{true};
    bool in_tool_call_{false};
    size_t sentences_emitted_{0};
    size_t total_bytes_emitted_{0};

    static constexpr size_t FIRST_CHUNK_MIN_BYTES = 6;    // e.g. "好的，" (2 UTF-8 Chinese characters)
    static constexpr size_t SUBSEQUENT_CHUNK_MIN_BYTES = 21; // e.g. 7 UTF-8 Chinese characters
    static constexpr size_t REPETITION_TRIGGER_BYTES = 12; // Consecutive 12-byte repeating pattern triggers watchdog
};

} // namespace audio_agent

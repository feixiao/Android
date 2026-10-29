#include "AudioAgentPipeline.hpp"
#include <iostream>
#include <chrono>

namespace audio_agent {

static int64_t GetCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

AudioAgentPipeline::AudioAgentPipeline(
    std::shared_ptr<ISpeechPerceptionEngine> perception_engine,
    std::shared_ptr<ILlmEngine> llm_engine,
    std::shared_ptr<ISpeechSynthesisEngine> tts_engine,
    PipelineObserver observer
) : perception_engine_(std::move(perception_engine)),
    llm_engine_(std::move(llm_engine)),
    tts_engine_(std::move(tts_engine)),
    observer_(std::move(observer)) {
    // 10 seconds capacity for ring buffers
    mic_ring_buffer_ = std::make_shared<LockFreeSPSCRingBuffer<float>>(DEFAULT_MIC_SAMPLE_RATE * 10);
    spk_ring_buffer_ = std::make_shared<LockFreeSPSCRingBuffer<float>>(DEFAULT_SPK_SAMPLE_RATE * 10);

    audio_io_ = std::make_unique<FullDuplexAudioIO>(
        mic_ring_buffer_,
        spk_ring_buffer_,
        DEFAULT_MIC_SAMPLE_RATE,
        DEFAULT_SPK_SAMPLE_RATE
    );
}

AudioAgentPipeline::~AudioAgentPipeline() {
    Stop();
}

bool AudioAgentPipeline::Start() {
    if (is_running_.load(std::memory_order_acquire)) {
        return true;
    }

    is_running_.store(true, std::memory_order_release);
    abort_flag_.store(false, std::memory_order_release);
    state_.store(AgentState::IDLE, std::memory_order_release);

    // Warmup prefix KV cache for LLM
    if (llm_engine_) {
        llm_engine_->WarmupPrefix(memory_.GetSystemPrompt());
    }

    // Start Audio IO Streams
    if (audio_io_) {
        audio_io_->StartStreams();
    }

    // Launch Background Worker Threads
    perception_thread_ = std::thread(&AudioAgentPipeline::PerceptionWorkerLoop, this);
    cognition_thread_ = std::thread(&AudioAgentPipeline::CognitionWorkerLoop, this);
    expression_thread_ = std::thread(&AudioAgentPipeline::ExpressionWorkerLoop, this);
    idle_monitor_thread_ = std::thread(&AudioAgentPipeline::IdleMonitorWorkerLoop, this);

    return true;
}

void AudioAgentPipeline::Stop() {
    if (!is_running_.load(std::memory_order_acquire)) return;

    is_running_.store(false, std::memory_order_release);
    abort_flag_.store(true, std::memory_order_release);

    watchdog_.Stop();

    // Wake all waiting threads
    query_cv_.notify_all();
    tts_cv_.notify_all();

    if (audio_io_) {
        audio_io_->StopStreams();
    }

    if (perception_thread_.joinable()) perception_thread_.join();
    if (cognition_thread_.joinable()) cognition_thread_.join();
    if (expression_thread_.joinable()) expression_thread_.join();
    if (idle_monitor_thread_.joinable()) idle_monitor_thread_.join();
}

void AudioAgentPipeline::SetState(AgentState new_state) {
    AgentState old_state = state_.exchange(new_state, std::memory_order_acq_rel);
    if (old_state != new_state && observer_.on_state_changed) {
        observer_.on_state_changed(new_state, old_state);
    }
}

void AudioAgentPipeline::TriggerBargeIn() {
    AgentState cur = state_.load(std::memory_order_acquire);
    if (cur == AgentState::THINKING || cur == AgentState::SPEAKING) {
        // Step 1: Raise atomic interrupt flag (notifies LLM streambuf and TTS callback)
        abort_flag_.store(true, std::memory_order_release);

        // Step 2: Instantly swap & clear pending TTS text queue
        {
            std::lock_guard<std::mutex> lock(tts_mtx_);
            std::queue<std::string> empty;
            std::swap(tts_queue_, empty);
        }

        // Step 3: Purge speaker ring buffer (hardware speaker immediately silences)
        if (spk_ring_buffer_) {
            spk_ring_buffer_->clear();
        }

        // Step 4: KV Cache Rollback (Section 4.3)
        std::string spoken_so_far;
        {
            std::lock_guard<std::mutex> lock(spoken_mtx_);
            spoken_so_far = spoken_text_accumulator_;
        }
        memory_.RollbackLastAssistantTurn(spoken_so_far);

        if (llm_engine_) {
            // Rough token estimate: 1 UTF-8 Chinese character ≈ 1.5 tokens
            size_t tokens_to_keep = spoken_so_far.size();
            llm_engine_->RollbackTokens(tokens_to_keep);
        }

        if (observer_.on_barge_in) {
            observer_.on_barge_in(spoken_so_far);
        }

        // Step 5: Transition state back to LISTENING
        SetState(AgentState::LISTENING);
    }
}

void AudioAgentPipeline::SubmitUserQuery(const std::string& user_query) {
    if (user_query.empty()) return;

    {
        std::lock_guard<std::mutex> lock(query_mtx_);
        query_queue_.push(user_query);
    }
    query_cv_.notify_one();
}

size_t AudioAgentPipeline::PushMicAudio(const float* data, size_t count) {
    if (!mic_ring_buffer_) return 0;
    return mic_ring_buffer_->write(data, count);
}

size_t AudioAgentPipeline::PullSpeakerAudio(float* dest, size_t count) {
    if (!spk_ring_buffer_) return 0;
    return spk_ring_buffer_->read(dest, count);
}

void AudioAgentPipeline::OnThermalStatusChanged(int thermal_status) {
    // Section 6.3: Moderate thermal throttling defense
    // 0 = NONE, 1 = LIGHT, 2 = MODERATE, 3 = SEVERE, 4 = CRITICAL
    if (thermal_status >= 2) {
        // Drop LLM worker thread priority or reduce tokens to prevent thermal throttling
        std::cerr << "[Thermal] Status changed to " << thermal_status << ", adjusting power profile.\n";
    }
}

void AudioAgentPipeline::PerceptionWorkerLoop() {
    // Bind perception thread to efficiency little cores (<150mW power budget)
    BindThreadToLittleCores(2);

    std::vector<float> frame(VAD_FRAME_SIZE_SAMPLES, 0.0f);

    while (is_running_.load(std::memory_order_acquire)) {
        if (!mic_ring_buffer_ || !perception_engine_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        size_t read_frames = mic_ring_buffer_->read(frame.data(), VAD_FRAME_SIZE_SAMPLES);
        if (read_frames < static_cast<size_t>(VAD_FRAME_SIZE_SAMPLES)) {
            // Not enough frames yet, yield audio slice (5ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        bool voice_active = perception_engine_->AcceptAudioChunk(frame.data(), VAD_FRAME_SIZE_SAMPLES);
        AgentState cur_state = state_.load(std::memory_order_acquire);

        if (voice_active) {
            if (cur_state == AgentState::THINKING || cur_state == AgentState::SPEAKING) {
                // Human voice detected during AI thinking or speaking -> Barge-in!
                TriggerBargeIn();
            } else if (cur_state == AgentState::IDLE) {
                // Start listening to user speech
                SetState(AgentState::LISTENING);
            }
        }

        if (state_.load(std::memory_order_acquire) == AgentState::LISTENING) {
            std::string partial = perception_engine_->GetRecognizedText(false);
            if (!partial.empty() && observer_.on_asr_partial) {
                observer_.on_asr_partial(partial);
            }

            if (perception_engine_->IsEndpointDetected()) {
                std::string final_query = perception_engine_->GetRecognizedText(true);
                perception_engine_->Reset();

                if (!final_query.empty()) {
                    if (observer_.on_asr_final) {
                        observer_.on_asr_final(final_query);
                    }
                    SubmitUserQuery(final_query);
                } else {
                    SetState(AgentState::IDLE);
                }
            }
        }
    }
}

void AudioAgentPipeline::CognitionWorkerLoop() {
    // Bind cognition thread to performance big cores
    BindThreadToBigCores(4);

    while (is_running_.load(std::memory_order_acquire)) {
        std::string query;
        {
            std::unique_lock<std::mutex> lock(query_mtx_);
            query_cv_.wait(lock, [this]() {
                return !query_queue_.empty() || !is_running_.load(std::memory_order_acquire);
            });

            if (!is_running_.load(std::memory_order_acquire)) break;
            query = std::move(query_queue_.front());
            query_queue_.pop();
        }

        abort_flag_.store(false, std::memory_order_release);
        SetState(AgentState::THINKING);

        {
            std::lock_guard<std::mutex> lock(spoken_mtx_);
            spoken_text_accumulator_.clear();
            current_user_query_ = query;
        }

        std::string full_assistant_response;

        // Set up streaming sentence chunker
        SentenceChunkerCallbacks callbacks;
        callbacks.on_sentence_ready = [this, &full_assistant_response](const std::string& sentence) {
            watchdog_.Feed(); // Feed TTFT watchdog on first chunk!
            full_assistant_response += sentence;
            EnqueueTtsSentence(sentence);
        };
        callbacks.on_tool_call = [this](const std::string& tool_json) {
            DispatchAsyncToolCall(tool_json);
        };
        callbacks.is_aborted = [this]() {
            return IsBargeInAborted();
        };
        callbacks.on_repetition_detected = [this]() {
            abort_flag_.store(true, std::memory_order_release);
            if (observer_.on_watchdog_fallback) {
                observer_.on_watchdog_fallback("N-gram Repetition Loop");
            }
            EnqueueTtsSentence(TTFTWatchdog::GetFallbackResponsePrompt());
        };

        StreamingSentenceChunker chunker(std::move(callbacks));
        std::ostream llm_stream(&chunker);

        // Start 3-second watchdog timer for First Packet (Section 6.3)
        watchdog_.Start(3000, [this]() {
            abort_flag_.store(true, std::memory_order_release);
            if (observer_.on_watchdog_fallback) {
                observer_.on_watchdog_fallback("TTFT 3s Timeout");
            }
            EnqueueTtsSentence(TTFTWatchdog::GetFallbackResponsePrompt());
        });

        // Format prompt with System Prompt + Memory
        std::string prompt = memory_.FormatPromptContext(query);

        if (llm_engine_) {
            llm_engine_->ResponseStream(prompt, &llm_stream);
        }

        chunker.Flush();
        watchdog_.Stop();

        // Record completed turn to memory
        if (!full_assistant_response.empty() && !IsBargeInAborted()) {
            memory_.AddTurn(query, full_assistant_response, false);
        }
    }
}

void AudioAgentPipeline::ExpressionWorkerLoop() {
    // Bind TTS thread to performance big cores
    BindThreadToBigCores(4);

    while (is_running_.load(std::memory_order_acquire)) {
        std::string sentence;
        {
            std::unique_lock<std::mutex> lock(tts_mtx_);
            tts_cv_.wait(lock, [this]() {
                return !tts_queue_.empty() || !is_running_.load(std::memory_order_acquire);
            });

            if (!is_running_.load(std::memory_order_acquire)) break;
            sentence = std::move(tts_queue_.front());
            tts_queue_.pop();
        }

        if (IsBargeInAborted()) {
            continue; // Dropped by Barge-in
        }

        SetState(AgentState::SPEAKING);
        if (observer_.on_tts_sentence_start) {
            observer_.on_tts_sentence_start(sentence);
        }

        bool aborted_during_tts = false;

        if (tts_engine_) {
            tts_engine_->SynthesizeStreaming(
                sentence,
                [this, &aborted_during_tts](const float* samples, int32_t count, float /*progress*/) -> int32_t {
                    if (IsBargeInAborted()) {
                        aborted_during_tts = true;
                        return 0; // Terminate Sherpa-onnx TTS synthesis loop immediately
                    }
                    if (spk_ring_buffer_) {
                        spk_ring_buffer_->write(samples, count);
                    }
                    return 1;
                }
            );
        }

        if (!aborted_during_tts && !IsBargeInAborted()) {
            std::lock_guard<std::mutex> lock(spoken_mtx_);
            spoken_text_accumulator_ += sentence;
        }

        // Check if all queued sentences are spoken
        {
            std::lock_guard<std::mutex> lock(tts_mtx_);
            if (tts_queue_.empty() && !IsBargeInAborted()) {
                // Wait for speaker ring buffer to empty
                while (spk_ring_buffer_ && spk_ring_buffer_->available_read() > 0 &&
                       !IsBargeInAborted() && is_running_.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (!IsBargeInAborted() && state_.load(std::memory_order_acquire) == AgentState::SPEAKING) {
                    SetState(AgentState::IDLE);
                }
            }
        }
    }
}

void AudioAgentPipeline::IdleMonitorWorkerLoop() {
    while (is_running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (state_.load(std::memory_order_acquire) == AgentState::IDLE) {
            memory_.MaybeTriggerIdleSummarization(GetCurrentTimeMs());
        }
    }
}

void AudioAgentPipeline::EnqueueTtsSentence(const std::string& sentence) {
    if (IsBargeInAborted() || sentence.empty()) return;

    {
        std::lock_guard<std::mutex> lock(tts_mtx_);
        tts_queue_.push(sentence);
    }
    tts_cv_.notify_one();
}

void AudioAgentPipeline::DispatchAsyncToolCall(const std::string& tool_json) {
    if (observer_.on_tool_call) {
        observer_.on_tool_call(tool_json);
    }
}

} // namespace audio_agent

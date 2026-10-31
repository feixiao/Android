#include "AudioAgentPipeline.hpp"
#include <iostream>
#include <chrono>

namespace audio_agent {

/**
 * @brief 获取当前系统毫秒级时间戳
 */
static int64_t GetCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

/**
 * @brief 构造函数：初始化三大引擎（感知、认知、表达）与全双工底层音频 IO
 * 
 * 环形缓冲区分配策略：
 * - 麦克风环形缓冲区（mic_ring_buffer_）：分配 10 秒容量（16kHz * 10 = 160000 Float），保障极端锁屏或高负载下录音不丢帧；
 * - 扬声器环形缓冲区（spk_ring_buffer_）：分配 10 秒容量（24kHz * 10 = 240000 Float），支撑大段流式 TTS 合成缓冲。
 */
AudioAgentPipeline::AudioAgentPipeline(
    std::shared_ptr<ISpeechPerceptionEngine> perception_engine,
    std::shared_ptr<ILlmEngine> llm_engine,
    std::shared_ptr<ISpeechSynthesisEngine> tts_engine,
    PipelineObserver observer,
    std::string system_prompt
) : perception_engine_(std::move(perception_engine)),
    llm_engine_(std::move(llm_engine)),
    tts_engine_(std::move(tts_engine)),
    observer_(std::move(observer)) {
    if (!system_prompt.empty()) {
        memory_.SetSystemPrompt(system_prompt);
    }
    // 为录音与放音分配 10 秒深度的无锁环形缓冲区
    mic_ring_buffer_ = std::make_shared<LockFreeSPSCRingBuffer<float>>(DEFAULT_MIC_SAMPLE_RATE * 10);
    spk_ring_buffer_ = std::make_shared<LockFreeSPSCRingBuffer<float>>(DEFAULT_SPK_SAMPLE_RATE * 10);

    // 实例化底层 Oboe 全双工音频控制器
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

/**
 * @brief 启动全双工 AI Agent 流水线
 * 
 * 启动时序：
 * 1. 预热大模型前缀缓存（Warmup Prefix KV Cache）；
 * 2. 启动 Oboe 硬件音频流；
 * 3. 并行拉起 4 个专属后台工作线程：
 *    - PerceptionWorkerLoop（感知线程，能效小核）
 *    - CognitionWorkerLoop（认知线程，性能大核）
 *    - ExpressionWorkerLoop（表达线程，性能大核）
 *    - IdleMonitorWorkerLoop（闲置监控线程，能效小核）
 */
bool AudioAgentPipeline::Start() {
    if (is_running_.load(std::memory_order_acquire)) {
        return true;
    }

    is_running_.store(true, std::memory_order_release);
    abort_flag_.store(false, std::memory_order_release);
    state_.store(AgentState::IDLE, std::memory_order_release);

    // 1. 预热系统提示词前缀 KV Cache
    if (llm_engine_) {
        llm_engine_->WarmupPrefix(memory_.GetSystemPrompt());
    }

    // 2. 启动底层 Oboe 录音与放音流
    if (audio_io_) {
        audio_io_->StartStreams();
    }

    // 3. 启动后台流水线工作线程池
    perception_thread_ = std::thread(&AudioAgentPipeline::PerceptionWorkerLoop, this);
    cognition_thread_ = std::thread(&AudioAgentPipeline::CognitionWorkerLoop, this);
    expression_thread_ = std::thread(&AudioAgentPipeline::ExpressionWorkerLoop, this);
    idle_monitor_thread_ = std::thread(&AudioAgentPipeline::IdleMonitorWorkerLoop, this);

    return true;
}

/**
 * @brief 停止流水线并优雅回收所有线程与资源
 */
void AudioAgentPipeline::Stop() {
    if (!is_running_.load(std::memory_order_acquire)) return;

    is_running_.store(false, std::memory_order_release);
    abort_flag_.store(true, std::memory_order_release);

    watchdog_.Stop();

    // 唤醒所有等待中的条件变量，防止线程挂起死锁
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

/**
 * @brief 临时挂起底层音频硬件流（例如兼容系统外部录音与语音识别应用）
 */
void AudioAgentPipeline::PauseAudioIO() {
    if (audio_io_) {
        audio_io_->StopStreams();
    }
    if (spk_ring_buffer_) {
        spk_ring_buffer_->clear();
    }
    if (mic_ring_buffer_) {
        mic_ring_buffer_->clear();
    }
}

/**
 * @brief 恢复底层音频硬件流
 */
bool AudioAgentPipeline::ResumeAudioIO() {
    if (!is_running_.load(std::memory_order_acquire)) return false;
    if (audio_io_) {
        bool ok = audio_io_->StartStreams();
        if (ok) {
            if (perception_engine_) {
                perception_engine_->Reset();
            }
            AgentState cur = state_.load(std::memory_order_acquire);
            if (cur == AgentState::SPEAKING || cur == AgentState::LISTENING) {
                SetState(AgentState::IDLE);
            }
        }
        return ok;
    }
    return false;
}

/**
 * @brief 原子切换状态机状态并通知上层观察者（UI 动效波形与状态球）
 */
void AudioAgentPipeline::SetState(AgentState new_state) {
    AgentState old_state = state_.exchange(new_state, std::memory_order_acq_rel);
    if (old_state != new_state && observer_.on_state_changed) {
        observer_.on_state_changed(new_state, old_state);
    }
}

/**
 * @brief 全双工核心打断控制逻辑（Barge-in Handler）
 * 
 * 5 步闪电响应机制（<5ms 极速闭环）：
 * 1. 升起原子打断标志（abort_flag_ = true）：通知正在运行的 LLM 流式输出与 TTS 合成循环立即退出；
 * 2. 瞬间清空待播报 TTS 文本队列；
 * 3. 瞬间清空扬声器环形缓冲区（spk_ring_buffer_->clear()）：硬件声卡立刻由于读不到数据而执行 zero-fill，喇叭瞬间静音；
 * 4. KV 缓存与对话记忆回滚：截断未播报内容，模型仅记住已发音的文字，防止产生幻觉；
 * 5. 状态机立即置为 LISTENING，开始聚焦收听用户的插话内容。
 */
void AudioAgentPipeline::TriggerBargeIn() {
    AgentState cur = state_.load(std::memory_order_acquire);
    if (cur == AgentState::THINKING || cur == AgentState::SPEAKING) {
        // 第 1 步：标记原子打断信号
        abort_flag_.store(true, std::memory_order_release);

        // 第 2 步：原子交换并丢弃未播报的文本队列
        {
            std::lock_guard<std::mutex> lock(tts_mtx_);
            std::queue<std::string> empty;
            std::swap(tts_queue_, empty);
        }

        // 第 3 步：清空底层扬声器环形缓冲区，扬声器在 5ms 内彻底静音
        if (spk_ring_buffer_) {
            spk_ring_buffer_->clear();
        }

        // 第 4 步：记忆回滚与大模型 KV 缓存截断
        std::string spoken_so_far;
        {
            std::lock_guard<std::mutex> lock(spoken_mtx_);
            spoken_so_far = spoken_text_accumulator_;
        }
        memory_.RollbackLastAssistantTurn(spoken_so_far);

        if (llm_engine_) {
            size_t tokens_to_keep = spoken_so_far.size();
            llm_engine_->RollbackTokens(tokens_to_keep);
        }

        if (observer_.on_barge_in) {
            observer_.on_barge_in(spoken_so_far);
        }

        // 第 5 步：状态切换回倾听中
        SetState(AgentState::LISTENING);
    }
}

/**
 * @brief 投递一条已完成识别的用户提问至认知处理队列
 */
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
    if (thermal_status >= 2) {
        std::cerr << "[Thermal] Status changed to " << thermal_status << ", adjusting power profile.\n";
    }
}

void AudioAgentPipeline::SetModelPath(const std::string& path) {
    if (perception_engine_) {
        perception_engine_->SetModelPath(path);
    }
}

void AudioAgentPipeline::SetLlmEngine(std::shared_ptr<ILlmEngine> llm_engine) {
    std::lock_guard<std::mutex> lock(query_mtx_);
    llm_engine_ = std::move(llm_engine);
}

LlmStats AudioAgentPipeline::GetLlmStats() const {
    if (llm_engine_) {
        return llm_engine_->GetLatestStats();
    }
    return {};
}

/**
 * @brief 感知处理循环（运行于独立线程，绑定能效小核）
 * 
 * 职责：
 * 1. 以 160 采样点（10ms）为基准步长从麦克风无锁缓冲区抽取音频；
 * 2. 驱动 VAD 语音活动检测：
 *    - 若在 THINKING/SPEAKING 阶段检测到人声 -> 立即触发 TriggerBargeIn() 打断；
 *    - 若在 IDLE 阶段检测到人声 -> 迁入 LISTENING；
 * 3. 驱动 ASR 语音识别引擎产出实时中间字（Partial）与句尾端点（Endpoint）。
 */
void AudioAgentPipeline::PerceptionWorkerLoop() {
    // 感知线程功耗极低（<150mW），严格绑定至小核运行
    BindThreadToLittleCores(2);

    std::vector<float> frame(VAD_FRAME_SIZE_SAMPLES, 0.0f);

    while (is_running_.load(std::memory_order_acquire)) {
        if (!mic_ring_buffer_ || !perception_engine_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        size_t read_frames = mic_ring_buffer_->read(frame.data(), VAD_FRAME_SIZE_SAMPLES);
        if (read_frames < static_cast<size_t>(VAD_FRAME_SIZE_SAMPLES)) {
            // 数据未就绪，让出时间片休眠 5ms
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        bool voice_active = perception_engine_->AcceptAudioChunk(frame.data(), VAD_FRAME_SIZE_SAMPLES);
        AgentState cur_state = state_.load(std::memory_order_acquire);

        if (voice_active) {
            if (cur_state == AgentState::THINKING || cur_state == AgentState::SPEAKING) {
                // 助手说话中被用户插话打断！
                TriggerBargeIn();
            } else if (cur_state == AgentState::IDLE) {
                // 待命状态下检测到人声，开始倾听
                SetState(AgentState::LISTENING);
            }
        }

        if (state_.load(std::memory_order_acquire) == AgentState::LISTENING) {
            // 上报流式实时识别中间字
            std::string partial = perception_engine_->GetRecognizedText(false);
            if (!partial.empty() && observer_.on_asr_partial) {
                observer_.on_asr_partial(partial);
            }

            // 检测到静音端点（约 320ms 连续静音，用户说话完毕）
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

/**
 * @brief 认知决策循环（运行于独立线程，绑定性能大核）
 * 
 * 职责：
 * 1. 从 query_queue_ 获取用户提问文本；
 * 2. 状态迁入 THINKING；
 * 3. 启动 TTFT 3 秒首字看门狗定时器；
 * 4. 组装多层记忆上下文（FormatPromptContext）；
 * 5. 将 StreamingSentenceChunker 作为流式缓冲区注入大模型，驱动流式推理；
 * 6. 首句断句生成后触发 EnqueueTtsSentence 并喂狗解除告警；
 * 7. 推理完毕后将完整对话更新入 L1 活跃记忆。
 */
void AudioAgentPipeline::CognitionWorkerLoop() {
    // LLM 推理需要最强算力，绑定至 Cortex-X / 大核
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
        // 意图仲裁器前置分析：提前提取确定性的端侧系统控制意图
        std::string fallback_tool_json = IntentArbiter::ResolveToolCall(query);


        // 构建流式断句器回调
        SentenceChunkerCallbacks callbacks;
        callbacks.on_sentence_ready = [this, &full_assistant_response](const std::string& sentence) {
            watchdog_.Feed(); // 首句生成成功，立即喂狗！
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

        // 启动 3 秒 TTFT 看门狗超时熔断
        watchdog_.Start(3000, [this]() {
            abort_flag_.store(true, std::memory_order_release);
            if (observer_.on_watchdog_fallback) {
                observer_.on_watchdog_fallback("TTFT 3s Timeout");
            }
            EnqueueTtsSentence(TTFTWatchdog::GetFallbackResponsePrompt());
        });

        // 拼接 ChatML Prompt 并触发模型流式生成
        std::string prompt = memory_.FormatPromptContext(query);

        if (llm_engine_) {
            llm_engine_->ResponseStream(prompt, &llm_stream);
        }

        chunker.Flush();
        watchdog_.Stop();
        // 意图补偿防漏调：若端侧小模型未能吐出 <|tool_call|>，而该 Query 包含明确的系统控制指令
        if (!chunker.HasDispatchedToolCall() && !fallback_tool_json.empty() && !IsBargeInAborted()) {
            std::cerr << "[IntentArbiter] Compensating missing tool call for query: '" << query
                      << "' -> dispatching: " << fallback_tool_json << std::endl;
            DispatchAsyncToolCall(fallback_tool_json);

            // 若模型未生成任何回复（如超时或未吐字），自动合成口语安抚回复推入 TTS
            if (full_assistant_response.empty()) {
                std::string reassurance;
                if (fallback_tool_json.find("start_nav") != std::string::npos) {
                    std::string dest = IntentArbiter::ExtractNavDestination(query);
                    reassurance = "好的，马上为您规划前往" + dest + "的最佳路线。";
                } else if (fallback_tool_json.find("set_ac_temp") != std::string::npos) {
                    reassurance = "好的，已为您调整空调温度。";
                } else if (fallback_tool_json.find("set_ac_power") != std::string::npos) {
                    reassurance = "好的，已为您设置空调。";
                } else if (fallback_tool_json.find("play_music") != std::string::npos) {
                    if (fallback_tool_json.find("pause") != std::string::npos) {
                        reassurance = "好的，已为您暂停音乐播放。";
                    } else if (fallback_tool_json.find("next") != std::string::npos) {
                        reassurance = "好的，已为您切到下一首。";
                    } else if (fallback_tool_json.find("previous") != std::string::npos) {
                        reassurance = "好的，已为您切回上一首。";
                    } else if (fallback_tool_json.find("resume") != std::string::npos) {
                        reassurance = "好的，继续播放音乐。";
                    } else {
                        reassurance = "好的，为您播放音乐。";
                    }
                } else if (fallback_tool_json.find("set_alarm") != std::string::npos) {
                    reassurance = "好的，已为您设定闹钟。";
                } else if (fallback_tool_json.find("query_weather") != std::string::npos) {
                    reassurance = "好的，正在为您查询天气。";
                }
                if (!reassurance.empty()) {
                    full_assistant_response = reassurance;
                    EnqueueTtsSentence(reassurance);
                }
            }
        }


        // 派发性能指标上报
        if (llm_engine_ && observer_.on_llm_stats) {
            observer_.on_llm_stats(llm_engine_->GetLatestStats());
        }

        // 若未被打断，记录该完整轮次至 L1 记忆
        if (!full_assistant_response.empty() && !IsBargeInAborted()) {
            memory_.AddTurn(query, full_assistant_response, false);
        }
    }
}

/**
 * @brief 表达合成循环（运行于独立线程，绑定性能大核）
 * 
 * 职责：
 * 1. 逐句从 tts_queue_ 中消费断句文本；
 * 2. 状态迁入 SPEAKING；
 * 3. 驱动 Sherpa-onnx VITS/Zipformer TTS 执行流式声学合成；
 * 4. 合成回调中以 Float PCM 块形式直接写入 spk_ring_buffer_；
 * 5. 精准累计已实际播报完成的文字，为打断回滚提供精准基准。
 */
void AudioAgentPipeline::ExpressionWorkerLoop() {
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
            continue; // 已被打断，丢弃队列中剩余待播报句子
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
                        return 0; // 立即中止 TTS 合成循环
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

        // 检查所有待发音句子是否已经全部消费并播放完成
        bool queue_empty = false;
        {
            std::lock_guard<std::mutex> lock(tts_mtx_);
            queue_empty = tts_queue_.empty();
        }

        if (queue_empty && !IsBargeInAborted()) {
            // 等待扬声器缓冲区内的剩余音频被声卡彻底消费完毕
            while (spk_ring_buffer_ && spk_ring_buffer_->available_read() > 0 &&
                   !IsBargeInAborted() && is_running_.load(std::memory_order_acquire) &&
                   audio_io_ && audio_io_->IsRunning()) {
                {
                    std::lock_guard<std::mutex> lock(tts_mtx_);
                    if (!tts_queue_.empty()) break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (spk_ring_buffer_ && (!audio_io_ || !audio_io_->IsRunning())) {
                spk_ring_buffer_->clear();
            }
            if (!IsBargeInAborted() && state_.load(std::memory_order_acquire) == AgentState::SPEAKING) {
                std::lock_guard<std::mutex> lock(tts_mtx_);
                if (tts_queue_.empty()) {
                    SetState(AgentState::IDLE);
                }
            }
        }
    }
}

/**
 * @brief 闲置监控后台线程：低频（每秒一次）巡检对话静默时长并触发记忆归纳
 */
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

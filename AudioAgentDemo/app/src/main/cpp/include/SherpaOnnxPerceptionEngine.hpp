#pragma once

#include "EngineInterfaces.hpp"
#include <string>
#include <memory>
#include <mutex>
#include <atomic>

// 前置声明 C API 句柄类型以降低头文件依赖耦合
struct SherpaOnnxOnlineRecognizer;
struct SherpaOnnxOnlineStream;
struct SherpaOnnxOnlineRecognizerResult;
struct SherpaOnnxOnlineRecognizerConfig;

namespace audio_agent {

/**
 * @brief Sherpa-onnx 端侧流式语音识别感知引擎
 * 
 * 职责：
 * 1. 动态加载 libsherpa-onnx-c-api.so 共享库，无缝绑定 C-API 符号；
 * 2. 加载端侧 Sherpa-onnx Zipformer 声学模型（encoder, decoder, joiner, tokens.txt）；
 * 3. 实时接收 16kHz Float 采样点音频块，驱动在线流式解码（OnlineStream）；
 * 4. 实时提取中间识别文字（Partial Text）与端点截断（Endpoint）；
 * 5. 用户说话结束时输出最终识别汉字文本并触发重置，无缝驱动端侧 LLM 决策。
 */
class SherpaOnnxPerceptionEngine : public ISpeechPerceptionEngine {
public:
    SherpaOnnxPerceptionEngine();
    ~SherpaOnnxPerceptionEngine() override;

    bool AcceptAudioChunk(const float* samples, int32_t count) override;
    bool IsEndpointDetected() override;
    std::string GetRecognizedText(bool is_final) override;
    void Reset() override;
    void SetModelPath(const std::string& path) override;

    bool IsModelLoaded() const { return is_model_loaded_.load(std::memory_order_acquire); }
    void SetManualText(const std::string& text);

private:
    bool InitSherpaSymbols();
    void UnloadModel();
    bool LoadZipformerModel(const std::string& dir_path);

private:
    void* lib_handle_{nullptr};
    const SherpaOnnxOnlineRecognizer* recognizer_{nullptr};
    const SherpaOnnxOnlineStream* stream_{nullptr};

    std::string model_dir_;
    std::atomic<bool> is_model_loaded_{false};
    std::atomic<bool> endpoint_detected_{false};

    // 线程安全互斥锁保护解码与文本获取
    mutable std::mutex recognizer_mtx_;

    // 兜底能量 VAD 状态变量
    bool in_speech_{false};
    int speech_frames_{0};
    int silence_frames_{0};
    std::string manual_text_;
    std::string last_partial_text_;

    // 动态符号函数指针定义
    const SherpaOnnxOnlineRecognizer* (*fn_create_recognizer_)(const SherpaOnnxOnlineRecognizerConfig*){nullptr};
    void (*fn_destroy_recognizer_)(const SherpaOnnxOnlineRecognizer*){nullptr};
    const SherpaOnnxOnlineStream* (*fn_create_stream_)(const SherpaOnnxOnlineRecognizer*){nullptr};
    void (*fn_destroy_stream_)(const SherpaOnnxOnlineStream*){nullptr};
    void (*fn_accept_waveform_)(const SherpaOnnxOnlineStream*, int32_t, const float*, int32_t){nullptr};
    int32_t (*fn_is_ready_)(const SherpaOnnxOnlineRecognizer*, const SherpaOnnxOnlineStream*){nullptr};
    void (*fn_decode_)(const SherpaOnnxOnlineRecognizer*, const SherpaOnnxOnlineStream*){nullptr};
    const SherpaOnnxOnlineRecognizerResult* (*fn_get_result_)(const SherpaOnnxOnlineRecognizer*, const SherpaOnnxOnlineStream*){nullptr};
    void (*fn_destroy_result_)(const SherpaOnnxOnlineRecognizerResult*){nullptr};
    int32_t (*fn_is_endpoint_)(const SherpaOnnxOnlineRecognizer*, const SherpaOnnxOnlineStream*){nullptr};
    void (*fn_reset_)(const SherpaOnnxOnlineRecognizer*, const SherpaOnnxOnlineStream*){nullptr};
};

} // namespace audio_agent

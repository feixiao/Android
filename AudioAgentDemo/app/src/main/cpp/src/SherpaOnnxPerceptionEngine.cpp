#include "SherpaOnnxPerceptionEngine.hpp"
#include "sherpa-onnx/c-api.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <dlfcn.h>

#if defined(__ANDROID__)
#include <android/log.h>
#define LOG_TAG "SherpaOnnxPerception"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) do { printf("[INFO] " __VA_ARGS__); printf("\n"); } while(0)
#define LOGW(...) do { printf("[WARN] " __VA_ARGS__); printf("\n"); } while(0)
#define LOGE(...) do { fprintf(stderr, "[ERROR] " __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#endif

namespace audio_agent {

SherpaOnnxPerceptionEngine::SherpaOnnxPerceptionEngine() {
    InitSherpaSymbols();
}

SherpaOnnxPerceptionEngine::~SherpaOnnxPerceptionEngine() {
    UnloadModel();
    if (lib_handle_) {
        dlclose(lib_handle_);
        lib_handle_ = nullptr;
    }
}

bool SherpaOnnxPerceptionEngine::InitSherpaSymbols() {
    if (lib_handle_) return true;

#if defined(__ANDROID__)
    // 动态加载已内置到 APK 中的 libsherpa-onnx-c-api.so
    lib_handle_ = dlopen("libsherpa-onnx-c-api.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib_handle_) {
        LOGW("dlopen libsherpa-onnx-c-api.so returned null: %s", dlerror());
        return false;
    }

#define BIND_SYMBOL(fn, name) \
    fn = reinterpret_cast<decltype(fn)>(dlsym(lib_handle_, name)); \
    if (!fn) { \
        LOGE("Failed to dlsym %s: %s", name, dlerror()); \
        dlclose(lib_handle_); \
        lib_handle_ = nullptr; \
        return false; \
    }

    BIND_SYMBOL(fn_create_recognizer_, "SherpaOnnxCreateOnlineRecognizer");
    BIND_SYMBOL(fn_destroy_recognizer_, "SherpaOnnxDestroyOnlineRecognizer");
    BIND_SYMBOL(fn_create_stream_, "SherpaOnnxCreateOnlineStream");
    BIND_SYMBOL(fn_destroy_stream_, "SherpaOnnxDestroyOnlineStream");
    BIND_SYMBOL(fn_accept_waveform_, "SherpaOnnxOnlineStreamAcceptWaveform");
    BIND_SYMBOL(fn_is_ready_, "SherpaOnnxIsOnlineStreamReady");
    BIND_SYMBOL(fn_decode_, "SherpaOnnxDecodeOnlineStream");
    BIND_SYMBOL(fn_get_result_, "SherpaOnnxGetOnlineStreamResult");
    BIND_SYMBOL(fn_destroy_result_, "SherpaOnnxDestroyOnlineRecognizerResult");
    BIND_SYMBOL(fn_is_endpoint_, "SherpaOnnxOnlineStreamIsEndpoint");
    BIND_SYMBOL(fn_reset_, "SherpaOnnxOnlineStreamReset");
#undef BIND_SYMBOL

    LOGI("Successfully loaded libsherpa-onnx-c-api.so and bound all C-API symbols!");
    return true;
#else
    return false;
#endif
}

void SherpaOnnxPerceptionEngine::UnloadModel() {
    std::lock_guard<std::mutex> lock(recognizer_mtx_);
    is_model_loaded_.store(false, std::memory_order_release);
    endpoint_detected_.store(false, std::memory_order_release);

    if (stream_ && fn_destroy_stream_) {
        fn_destroy_stream_(stream_);
        stream_ = nullptr;
    }
    if (recognizer_ && fn_destroy_recognizer_) {
        fn_destroy_recognizer_(recognizer_);
        recognizer_ = nullptr;
    }
}

bool SherpaOnnxPerceptionEngine::LoadZipformerModel(const std::string& dir_path) {
    if (!InitSherpaSymbols()) {
        LOGW("Cannot load Zipformer model: Sherpa C-API library unavailable");
        return false;
    }

    std::string enc = dir_path + "/encoder-epoch-99-avg-1.int8.onnx";
    std::string dec = dir_path + "/decoder-epoch-99-avg-1.onnx";
    std::string joiner = dir_path + "/joiner-epoch-99-avg-1.int8.onnx";
    std::string tokens = dir_path + "/tokens.txt";

    // 校验四个必要模型权重文件是否存在且有效
    auto file_ok = [](const std::string& f) {
        std::ifstream ifs(f, std::ios::binary);
        return ifs.good();
    };

    if (!file_ok(enc) || !file_ok(dec) || !file_ok(joiner) || !file_ok(tokens)) {
        LOGW("Zipformer model files incomplete in directory: %s", dir_path.c_str());
        return false;
    }

    UnloadModel();

    std::lock_guard<std::mutex> lock(recognizer_mtx_);

    SherpaOnnxOnlineRecognizerConfig config;
    std::memset(&config, 0, sizeof(config));

    // 16kHz 黄金采样率，80 维 Fbank 特征
    config.feat_config.sample_rate = 16000;
    config.feat_config.feature_dim = 80;

    config.model_config.transducer.encoder = enc.c_str();
    config.model_config.transducer.decoder = dec.c_str();
    config.model_config.transducer.joiner = joiner.c_str();
    config.model_config.tokens = tokens.c_str();
    config.model_config.num_threads = 2; // 双核并行
    config.model_config.provider = "cpu";

    config.decoding_method = "greedy_search";
    config.max_active_paths = 4;
    config.enable_endpoint = 1;
    config.rule1_min_trailing_silence = 2.4f;
    config.rule2_min_trailing_silence = VAD_SILENCE_THRESHOLD_SEC; // 静音判定句末落音 (EngineInterfaces.hpp)
    config.rule3_min_utterance_length = 30.0f;

    LOGI("Creating SherpaOnnxOnlineRecognizer with encoder: %s", enc.c_str());
    recognizer_ = fn_create_recognizer_(&config);
    if (!recognizer_) {
        LOGE("Failed to create SherpaOnnxOnlineRecognizer");
        return false;
    }

    stream_ = fn_create_stream_(recognizer_);
    if (!stream_) {
        LOGE("Failed to create SherpaOnnxOnlineStream");
        fn_destroy_recognizer_(recognizer_);
        recognizer_ = nullptr;
        return false;
    }

    model_dir_ = dir_path;
    is_model_loaded_.store(true, std::memory_order_release);
    LOGI("SherpaOnnxPerceptionEngine: Zipformer ASR model successfully loaded and ready!");
    return true;
}

void SherpaOnnxPerceptionEngine::SetModelPath(const std::string& path) {
    if (path.empty()) {
        UnloadModel();
        return;
    }
    LoadZipformerModel(path);
}

void SherpaOnnxPerceptionEngine::SetManualText(const std::string& text) {
    std::lock_guard<std::mutex> lock(recognizer_mtx_);
    manual_text_ = text;
    if (!text.empty()) {
        endpoint_detected_.store(true, std::memory_order_release);
    }
}

bool SherpaOnnxPerceptionEngine::AcceptAudioChunk(const float* samples, int32_t count) {
    float energy = 0.0f;
    for (int i = 0; i < count; ++i) {
        energy += std::abs(samples[i]);
    }
    float avg_energy = count > 0 ? (energy / count) : 0.0f;
    bool is_voice = avg_energy > 0.025f;

    if (is_model_loaded_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(recognizer_mtx_);
        if (stream_ && recognizer_) {
            // 将 16kHz PCM 采样点灌入在线解码流
            fn_accept_waveform_(stream_, 16000, samples, count);

            // 驱动声学网络解码前向推理
            while (fn_is_ready_(recognizer_, stream_)) {
                fn_decode_(recognizer_, stream_);
            }

            // 检查端点静音截断
            if (fn_is_endpoint_(recognizer_, stream_) != 0) {
                endpoint_detected_.store(true, std::memory_order_release);
                const auto* r = fn_get_result_(recognizer_, stream_);
                if (r && r->text && strlen(r->text) > 0) {
                    LOGI("Sherpa-onnx neural endpoint detected! Current text: '%s'", r->text);
                }
                if (r) {
                    fn_destroy_result_(r);
                }
            }
        }
    }

    // 辅助 VAD 状态跟踪
    if (is_voice) {
        speech_frames_++;
        silence_frames_ = 0;
        if (speech_frames_ >= 3) {
            in_speech_ = true;
        }
    } else {
        if (in_speech_) {
            silence_frames_++;
            // 根据 VAD_SILENCE_THRESHOLD_SEC 动态计算所需连续静音帧数 (例如 800ms / 32ms = 25 帧)
            constexpr int kEndpointSilenceFrames = static_cast<int>(
                VAD_SILENCE_THRESHOLD_SEC * DEFAULT_MIC_SAMPLE_RATE / VAD_FRAME_SIZE_SAMPLES
            );
            if (silence_frames_ >= kEndpointSilenceFrames) {
                in_speech_ = false;
                endpoint_detected_.store(true, std::memory_order_release);
                speech_frames_ = 0;
                silence_frames_ = 0;
            }
        }
    }

    return is_voice || in_speech_;
}

bool SherpaOnnxPerceptionEngine::IsEndpointDetected() {
    return endpoint_detected_.load(std::memory_order_acquire);
}

std::string SherpaOnnxPerceptionEngine::GetRecognizedText(bool is_final) {
    std::lock_guard<std::mutex> lock(recognizer_mtx_);

    if (!manual_text_.empty() && is_final) {
        std::string res = manual_text_;
        manual_text_.clear();
        endpoint_detected_.store(false, std::memory_order_release);
        return res;
    }

    if (is_model_loaded_.load(std::memory_order_acquire) && recognizer_ && stream_) {
        const auto* r = fn_get_result_(recognizer_, stream_);
        std::string text;
        if (r && r->text) {
            text = r->text;
        }
        if (r) {
            fn_destroy_result_(r);
        }

        if (is_final) {
            endpoint_detected_.store(false, std::memory_order_release);
            last_partial_text_.clear();
            if (!text.empty()) {
                LOGI("Final recognized ASR query emitted: '%s'", text.c_str());
            }
            return text;
        } else {
            if (!text.empty()) {
                last_partial_text_ = text;
                return text;
            }
            return in_speech_ ? "正在倾听..." : "";
        }
    }

    if (is_final) {
        endpoint_detected_.store(false, std::memory_order_release);
        return "";
    }
    return in_speech_ ? "正在倾听..." : "";
}

void SherpaOnnxPerceptionEngine::Reset() {
    std::lock_guard<std::mutex> lock(recognizer_mtx_);
    endpoint_detected_.store(false, std::memory_order_release);
    in_speech_ = false;
    speech_frames_ = 0;
    silence_frames_ = 0;
    manual_text_.clear();
    last_partial_text_.clear();

    if (is_model_loaded_.load(std::memory_order_acquire) && recognizer_ && stream_) {
        fn_reset_(recognizer_, stream_);
    }
}

} // namespace audio_agent

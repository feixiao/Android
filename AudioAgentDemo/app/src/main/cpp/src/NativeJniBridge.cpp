#include "NativeJniBridge.hpp"
#include "AudioAgentPipeline.hpp"
#include "CpuAffinity.hpp"

#if defined(__ANDROID__)
#include <android/log.h>
#define TAG "AudioAgentNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#else
#include <iostream>
#define LOGI(...) std::cout << "[AudioAgentNative] " << __VA_ARGS__ << std::endl
#define LOGE(...) std::cerr << "[AudioAgentNative ERROR] " << __VA_ARGS__ << std::endl
#endif

namespace audio_agent {

// Simulated/Demo engines for out-of-the-box operation on Android without massive weight downloads
class DemoPerceptionEngine : public ISpeechPerceptionEngine {
public:
    bool AcceptAudioChunk(const float* samples, int32_t count) override {
        float energy = 0.0f;
        for (int i = 0; i < count; ++i) energy += std::abs(samples[i]);
        float avg_energy = count > 0 ? (energy / count) : 0.0f;

        // Increased threshold to 0.035f so background room noise does not falsely trigger
        bool is_voice = avg_energy > 0.035f;
        if (is_voice) {
            speech_frames_++;
            silence_frames_ = 0;
            if (speech_frames_ >= 3) {
                in_speech_ = true;
                endpoint_ = false;
            }
        } else {
            if (in_speech_) {
                silence_frames_++;
                // ~320ms silence endpointing per Section 1.3 Latency Budget
                if (silence_frames_ >= 10) {
                    endpoint_ = true;
                    in_speech_ = false;
                    speech_frames_ = 0;
                    silence_frames_ = 0;
                }
            }
        }
        return is_voice;
    }

    bool IsEndpointDetected() override {
        return endpoint_;
    }

    std::string GetRecognizedText(bool is_final) override {
        if (is_final) {
            endpoint_ = false;
            if (!manual_text_.empty()) {
                std::string res = manual_text_;
                manual_text_.clear();
                return res;
            }
            // Do NOT fabricate hallucinated queries on noise.
            return "";
        }
        return in_speech_ ? "正在倾听..." : "";
    }

    void Reset() override {
        endpoint_ = false;
        in_speech_ = false;
        speech_frames_ = 0;
        silence_frames_ = 0;
        manual_text_.clear();
    }

private:
    std::string manual_text_;
    bool endpoint_{false};
    bool in_speech_{false};
    int speech_frames_{0};
    int silence_frames_{0};
};

class DemoLlmEngine : public ILlmEngine {
public:
    void ResponseStream(const std::string& prompt, std::ostream* out_stream) override {
        if (!out_stream) return;

        // Extract current turn query from multi-tier conversation context
        std::string query = prompt;
        size_t pos = prompt.rfind("<|im_start|>user\n");
        if (pos != std::string::npos) {
            size_t start = pos + std::string("<|im_start|>user\n").length();
            size_t end = prompt.find("<|im_end|>", start);
            if (end != std::string::npos) {
                query = prompt.substr(start, end - start);
            } else {
                query = prompt.substr(start);
            }
        }

        // Rich conversational responses matching Section 5.2 SFT targets
        if (query.find("你好") != std::string::npos || query.find("在吗") != std::string::npos || query.find("嗨") != std::string::npos) {
            *out_stream << "你好呀！我是本地全双工AI语音助手，支持随时插话打断。请问今天有什么我可以帮您的？";
            out_stream->flush();
        } else if (query.find("天气") != std::string::npos || query.find("气温") != std::string::npos || query.find("下雨") != std::string::npos) {
            *out_stream << "今天天气晴朗，";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "气温二十二度，微风舒适，非常适合外出。";
            out_stream->flush();
        } else if (query.find("空调") != std::string::npos || query.find("温度") != std::string::npos) {
            *out_stream << "没问题，";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "已调至二十四度。<|tool_call|>{\"action\":\"set_ac_temp\",\"value\":24}<|/tool_call|>";
            out_stream->flush();
        } else if (query.find("导航") != std::string::npos || query.find("去西湖") != std::string::npos || query.find("路线") != std::string::npos) {
            *out_stream << "好的，";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "马上为您规划最佳路线。<|tool_call|>{\"action\":\"start_nav\",\"dest\":\"杭州西湖文化广场\"}<|/tool_call|>";
            out_stream->flush();
        } else if (query.find("蓝") != std::string::npos || query.find("天空") != std::string::npos || query.find("为什么") != std::string::npos) {
            *out_stream << "这叫瑞利散射。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "简单说，阳光里的蓝光波长短，穿过大气层时容易被空气分子散射，所以咱们抬头看到的天空就是蓝色的。";
            out_stream->flush();
        } else {
            *out_stream << "明白，";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "我已接收到您的指令。我是端侧纯本地运行的智能助手，支持随时插话打断与异步工具调用。";
            out_stream->flush();
        }
    }

    void RollbackTokens(size_t /*tokens_to_keep*/) override {}
    bool WarmupPrefix(const std::string& /*system_prompt*/) override { return true; }
};

class DemoTtsEngine : public ISpeechSynthesisEngine {
public:
    bool SynthesizeStreaming(const std::string& text, AudioChunkCallback callback) override {
        const int CHUNK_SIZE = 480; // 20ms at 24kHz
        std::vector<float> pcm(CHUNK_SIZE, 0.0f);
        size_t num_chunks = std::max<size_t>(3, (text.size() / 3) * 2);
        if (num_chunks > 15) num_chunks = 15;

        for (size_t c = 0; c < num_chunks; ++c) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            float freq = 480.0f;
            for (int i = 0; i < CHUNK_SIZE; ++i) {
                float t = static_cast<float>(c * CHUNK_SIZE + i) / 24000.0f;
                pcm[i] = 0.04f * std::sin(2.0f * 3.14159265f * freq * t);
            }
            int rc = callback(pcm.data(), CHUNK_SIZE, static_cast<float>(c + 1) / num_chunks);
            if (rc == 0) return false; // Aborted by Barge-in
        }
        return true;
    }

    int32_t GetSampleRate() const override { return 24000; }
};

// Bridge Context wrapping pipeline and Java VM references
struct JniBridgeContext {
    JavaVM* jvm{nullptr};
    jobject listener_global_ref{nullptr};
    std::shared_ptr<AudioAgentPipeline> pipeline;

    jmethodID mid_on_state_changed{nullptr};
    jmethodID mid_on_asr_partial{nullptr};
    jmethodID mid_on_asr_final{nullptr};
    jmethodID mid_on_tts_sentence_start{nullptr};
    jmethodID mid_on_tool_call{nullptr};
    jmethodID mid_on_barge_in{nullptr};
    jmethodID mid_on_watchdog_fallback{nullptr};

    void CallVoidMethodWithEnv(std::function<void(JNIEnv*)> fn) {
        if (!jvm || !listener_global_ref) return;

        JNIEnv* env = nullptr;
        bool need_detach = false;
        jint res = jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (res == JNI_EDETACHED) {
#if defined(__ANDROID__)
            if (jvm->AttachCurrentThread(&env, nullptr) == 0) {
                need_detach = true;
            }
#else
            if (jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) == 0) {
                need_detach = true;
            }
#endif
        }

        if (env) {
            fn(env);
            if (need_detach) {
                jvm->DetachCurrentThread();
            }
        }
    }
};

} // namespace audio_agent

using namespace audio_agent;

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeCreate(
    JNIEnv* env,
    jobject thiz,
    jstring system_prompt
) {
    auto* ctx = new JniBridgeContext();
    env->GetJavaVM(&ctx->jvm);
    ctx->listener_global_ref = env->NewGlobalRef(thiz);

    jclass cls = env->GetObjectClass(thiz);
    ctx->mid_on_state_changed = env->GetMethodID(cls, "onNativeStateChanged", "(II)V");
    ctx->mid_on_asr_partial = env->GetMethodID(cls, "onNativeAsrPartial", "(Ljava/lang/String;)V");
    ctx->mid_on_asr_final = env->GetMethodID(cls, "onNativeAsrFinal", "(Ljava/lang/String;)V");
    ctx->mid_on_tts_sentence_start = env->GetMethodID(cls, "onNativeTtsSentenceStart", "(Ljava/lang/String;)V");
    ctx->mid_on_tool_call = env->GetMethodID(cls, "onNativeToolCall", "(Ljava/lang/String;)V");
    ctx->mid_on_barge_in = env->GetMethodID(cls, "onNativeBargeIn", "(Ljava/lang/String;)V");
    ctx->mid_on_watchdog_fallback = env->GetMethodID(cls, "onNativeWatchdogFallback", "(Ljava/lang/String;)V");

    const char* sys_prompt_cstr = env->GetStringUTFChars(system_prompt, nullptr);
    std::string sys_prompt = sys_prompt_cstr ? sys_prompt_cstr : "";
    env->ReleaseStringUTFChars(system_prompt, sys_prompt_cstr);

    auto perception = std::make_shared<DemoPerceptionEngine>();
    auto llm = std::make_shared<DemoLlmEngine>();
    auto tts = std::make_shared<DemoTtsEngine>();

    PipelineObserver observer;
    observer.on_state_changed = [ctx](AgentState new_state, AgentState old_state) {
        ctx->CallVoidMethodWithEnv([ctx, new_state, old_state](JNIEnv* env) {
            if (ctx->mid_on_state_changed) {
                env->CallVoidMethod(
                    ctx->listener_global_ref,
                    ctx->mid_on_state_changed,
                    static_cast<jint>(new_state),
                    static_cast<jint>(old_state)
                );
            }
        });
    };

    observer.on_asr_partial = [ctx](const std::string& partial) {
        ctx->CallVoidMethodWithEnv([ctx, partial](JNIEnv* env) {
            if (ctx->mid_on_asr_partial) {
                jstring js = env->NewStringUTF(partial.c_str());
                env->CallVoidMethod(ctx->listener_global_ref, ctx->mid_on_asr_partial, js);
                env->DeleteLocalRef(js);
            }
        });
    };

    observer.on_asr_final = [ctx](const std::string& final_text) {
        ctx->CallVoidMethodWithEnv([ctx, final_text](JNIEnv* env) {
            if (ctx->mid_on_asr_final) {
                jstring js = env->NewStringUTF(final_text.c_str());
                env->CallVoidMethod(ctx->listener_global_ref, ctx->mid_on_asr_final, js);
                env->DeleteLocalRef(js);
            }
        });
    };

    observer.on_tts_sentence_start = [ctx](const std::string& sentence) {
        ctx->CallVoidMethodWithEnv([ctx, sentence](JNIEnv* env) {
            if (ctx->mid_on_tts_sentence_start) {
                jstring js = env->NewStringUTF(sentence.c_str());
                env->CallVoidMethod(ctx->listener_global_ref, ctx->mid_on_tts_sentence_start, js);
                env->DeleteLocalRef(js);
            }
        });
    };

    observer.on_tool_call = [ctx](const std::string& tool_json) {
        ctx->CallVoidMethodWithEnv([ctx, tool_json](JNIEnv* env) {
            if (ctx->mid_on_tool_call) {
                jstring js = env->NewStringUTF(tool_json.c_str());
                env->CallVoidMethod(ctx->listener_global_ref, ctx->mid_on_tool_call, js);
                env->DeleteLocalRef(js);
            }
        });
    };

    observer.on_barge_in = [ctx](const std::string& spoken) {
        ctx->CallVoidMethodWithEnv([ctx, spoken](JNIEnv* env) {
            if (ctx->mid_on_barge_in) {
                jstring js = env->NewStringUTF(spoken.c_str());
                env->CallVoidMethod(ctx->listener_global_ref, ctx->mid_on_barge_in, js);
                env->DeleteLocalRef(js);
            }
        });
    };

    observer.on_watchdog_fallback = [ctx](const std::string& reason) {
        ctx->CallVoidMethodWithEnv([ctx, reason](JNIEnv* env) {
            if (ctx->mid_on_watchdog_fallback) {
                jstring js = env->NewStringUTF(reason.c_str());
                env->CallVoidMethod(ctx->listener_global_ref, ctx->mid_on_watchdog_fallback, js);
                env->DeleteLocalRef(js);
            }
        });
    };

    ctx->pipeline = std::make_shared<AudioAgentPipeline>(perception, llm, tts, std::move(observer));
    return reinterpret_cast<jlong>(ctx);
}

JNIEXPORT jboolean JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeStart(
    JNIEnv* /*env*/,
    jobject /*thiz*/,
    jlong handle
) {
    auto* ctx = reinterpret_cast<JniBridgeContext*>(handle);
    if (!ctx || !ctx->pipeline) return JNI_FALSE;
    return ctx->pipeline->Start() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeStop(
    JNIEnv* /*env*/,
    jobject /*thiz*/,
    jlong handle
) {
    auto* ctx = reinterpret_cast<JniBridgeContext*>(handle);
    if (ctx && ctx->pipeline) {
        ctx->pipeline->Stop();
    }
}

JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeTriggerBargeIn(
    JNIEnv* /*env*/,
    jobject /*thiz*/,
    jlong handle
) {
    auto* ctx = reinterpret_cast<JniBridgeContext*>(handle);
    if (ctx && ctx->pipeline) {
        ctx->pipeline->TriggerBargeIn();
    }
}

JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeSubmitUserQuery(
    JNIEnv* env,
    jobject /*thiz*/,
    jlong handle,
    jstring query
) {
    auto* ctx = reinterpret_cast<JniBridgeContext*>(handle);
    if (!ctx || !ctx->pipeline || !query) return;

    const char* q_str = env->GetStringUTFChars(query, nullptr);
    std::string query_cpp = q_str ? q_str : "";
    env->ReleaseStringUTFChars(query, q_str);

    ctx->pipeline->SubmitUserQuery(query_cpp);
}

JNIEXPORT jint JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeGetState(
    JNIEnv* /*env*/,
    jobject /*thiz*/,
    jlong handle
) {
    auto* ctx = reinterpret_cast<JniBridgeContext*>(handle);
    if (!ctx || !ctx->pipeline) return -1;
    return static_cast<jint>(ctx->pipeline->GetState());
}

JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeOnThermalChanged(
    JNIEnv* /*env*/,
    jobject /*thiz*/,
    jlong handle,
    jint thermal_status
) {
    auto* ctx = reinterpret_cast<JniBridgeContext*>(handle);
    if (ctx && ctx->pipeline) {
        ctx->pipeline->OnThermalStatusChanged(thermal_status);
    }
}

JNIEXPORT jintArray JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeGetTopPerformanceCores(
    JNIEnv* env,
    jclass /*clazz*/,
    jint top_n
) {
    auto cores = DetectTopPerformanceCores(top_n > 0 ? top_n : 4);
    jintArray result = env->NewIntArray(static_cast<jsize>(cores.size()));
    if (!cores.empty()) {
        env->SetIntArrayRegion(result, 0, static_cast<jsize>(cores.size()), reinterpret_cast<const jint*>(cores.data()));
    }
    return result;
}

JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeDestroy(
    JNIEnv* env,
    jobject /*thiz*/,
    jlong handle
) {
    auto* ctx = reinterpret_cast<JniBridgeContext*>(handle);
    if (ctx) {
        if (ctx->pipeline) {
            ctx->pipeline->Stop();
        }
        if (ctx->listener_global_ref) {
            env->DeleteGlobalRef(ctx->listener_global_ref);
        }
        delete ctx;
    }
}

} // extern "C"

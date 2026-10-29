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
        return (energy / count) > 0.02f;
    }

    bool IsEndpointDetected() override {
        return endpoint_;
    }

    std::string GetRecognizedText(bool /*is_final*/) override {
        return text_;
    }

    void Reset() override {
        endpoint_ = false;
        text_.clear();
    }

    void SetRecognized(const std::string& t, bool endpoint) {
        text_ = t;
        endpoint_ = endpoint;
    }

private:
    std::string text_;
    bool endpoint_{false};
};

class DemoLlmEngine : public ILlmEngine {
public:
    void ResponseStream(const std::string& query, std::ostream* out_stream) override {
        if (!out_stream) return;

        // Simulate intelligent response with fast 6-byte first sentence & tool call support
        if (query.find("导航") != std::string::npos || query.find("去") != std::string::npos) {
            *out_stream << "好的，";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            *out_stream << "马上为您规划最佳路线。<|tool_call|>{\"action\":\"start_nav\",\"dest\":\"杭州西湖文化广场\"}<|/tool_call|>";
            out_stream->flush();
        } else if (query.find("空调") != std::string::npos || query.find("温度") != std::string::npos) {
            *out_stream << "没问题，";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            *out_stream << "已调至二十四度。<|tool_call|>{\"action\":\"set_ac_temp\",\"value\":24}<|/tool_call|>";
            out_stream->flush();
        } else {
            *out_stream << "明白，";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            *out_stream << "我已接收到您的语音指令。我是端侧纯本地运行的全双工助手，支持随时插话打断。";
            out_stream->flush();
        }
    }

    void RollbackTokens(size_t /*tokens_to_keep*/) override {}
    bool WarmupPrefix(const std::string& /*system_prompt*/) override { return true; }
};

class DemoTtsEngine : public ISpeechSynthesisEngine {
public:
    bool SynthesizeStreaming(const std::string& /*text*/, AudioChunkCallback callback) override {
        const int CHUNK_SIZE = 480; // 20ms at 24kHz
        std::vector<float> pcm(CHUNK_SIZE, 0.2f);
        // Synthesize 5 chunks (100ms audio)
        for (int i = 0; i < 5; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            int rc = callback(pcm.data(), CHUNK_SIZE, (i + 1) * 0.2f);
            if (rc == 0) return false;
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

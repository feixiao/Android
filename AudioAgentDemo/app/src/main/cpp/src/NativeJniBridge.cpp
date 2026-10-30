#include "NativeJniBridge.hpp"
#include "AudioAgentPipeline.hpp"
#include "CpuAffinity.hpp"
#include "MnnLlmEngine.hpp"
#include "MockCloudLlmEngine.hpp"
#include "SherpaOnnxPerceptionEngine.hpp"
#include <android/log.h>

#define TAG "AudioAgentNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace audio_agent {

/**
 * @brief 仿真语音合成引擎（DemoTtsEngine）
 * 
 * 作用：以 20ms（480 采样点）为周期产生静音帧，用于驱动 Oboe 播音时钟步进，免除刺耳正弦蜂鸣声。
 */
class DemoTtsEngine : public ISpeechSynthesisEngine {
public:
    bool SynthesizeStreaming(const std::string& text, AudioChunkCallback callback) override {
        const int CHUNK_SIZE = 480; // 24kHz 下 20ms 采样数
        std::vector<float> pcm(CHUNK_SIZE, 0.0f);
        size_t num_chunks = std::max<size_t>(3, (text.size() / 3) * 2);
        if (num_chunks > 15) num_chunks = 15;

        for (size_t c = 0; c < num_chunks; ++c) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            int rc = callback(pcm.data(), CHUNK_SIZE, static_cast<float>(c + 1) / num_chunks);
            if (rc == 0) return false; // 用户插话打断，立即退出合成
        }
        return true;
    }

    int32_t GetSampleRate() const override { return 24000; }
};

/**
 * @brief JNI 桥接上下文：持有 JavaVM 指针、回调 GlobalRef 与流水线对象实例
 */
struct JniBridgeContext {
    JavaVM* jvm{nullptr};
    jobject listener_global_ref{nullptr};
    std::shared_ptr<AudioAgentPipeline> pipeline;
    std::shared_ptr<MnnLlmEngine> mnn_llm;
    std::shared_ptr<MockCloudLlmEngine> cloud_llm;
    jmethodID mid_on_llm_stats{nullptr};

    jmethodID mid_on_state_changed{nullptr};
    jmethodID mid_on_asr_partial{nullptr};
    jmethodID mid_on_asr_final{nullptr};
    jmethodID mid_on_tts_sentence_start{nullptr};
    jmethodID mid_on_tool_call{nullptr};
    jmethodID mid_on_barge_in{nullptr};
    jmethodID mid_on_watchdog_fallback{nullptr};

    /**
     * @brief 跨线程安全执行 JNI 回调辅助函数
     * 
     * 解决底层 C++ 工作线程（如 Perception/Cognition/Expression）无 JNIEnv 环境的问题：
     * 自动通过 JavaVM::AttachCurrentThread 挂载线程并在执行完后 Detach。
     */
    void CallVoidMethodWithEnv(std::function<void(JNIEnv*)> fn) {
        if (!jvm || !listener_global_ref) return;

        JNIEnv* env = nullptr;
        bool need_detach = false;
        jint res = jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (res == JNI_EDETACHED) {
            if (jvm->AttachCurrentThread(&env, nullptr) == 0) {
                need_detach = true;
            }
        }

        if (env) {
            fn(env);
            if (env->ExceptionCheck()) {
                env->ExceptionDescribe();
                env->ExceptionClear();
            }
            if (need_detach) {
                jvm->DetachCurrentThread();
            }
        }
    }
};

} // namespace audio_agent

using namespace audio_agent;

extern "C" {

/**
 * @brief JNI 导出：创建全双工 Agent 核心流水线实例并绑定上层 Java 回调
 */
JNIEXPORT jlong JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeCreate(
    JNIEnv* env,
    jobject thiz,
    jstring system_prompt
) {
    auto* ctx = new JniBridgeContext();
    env->GetJavaVM(&ctx->jvm);
    ctx->listener_global_ref = env->NewGlobalRef(thiz);

    // 缓存 Java 层回调 MethodID
    jclass cls = env->GetObjectClass(thiz);
    ctx->mid_on_state_changed = env->GetMethodID(cls, "onNativeStateChanged", "(II)V");
    ctx->mid_on_asr_partial = env->GetMethodID(cls, "onNativeAsrPartial", "(Ljava/lang/String;)V");
    ctx->mid_on_asr_final = env->GetMethodID(cls, "onNativeAsrFinal", "(Ljava/lang/String;)V");
    ctx->mid_on_tts_sentence_start = env->GetMethodID(cls, "onNativeTtsSentenceStart", "(Ljava/lang/String;)V");
    ctx->mid_on_tool_call = env->GetMethodID(cls, "onNativeToolCall", "(Ljava/lang/String;)V");
    ctx->mid_on_barge_in = env->GetMethodID(cls, "onNativeBargeIn", "(Ljava/lang/String;)V");
    ctx->mid_on_watchdog_fallback = env->GetMethodID(cls, "onNativeWatchdogFallback", "(Ljava/lang/String;)V");
    ctx->mid_on_llm_stats = env->GetMethodID(cls, "onNativeLlmStats", "(Ljava/lang/String;)V");

    std::string sys_prompt;
    if (system_prompt) {
        const char* sys_prompt_cstr = env->GetStringUTFChars(system_prompt, nullptr);
        if (sys_prompt_cstr) {
            sys_prompt = sys_prompt_cstr;
            env->ReleaseStringUTFChars(system_prompt, sys_prompt_cstr);
        }
    }

    auto perception = std::make_shared<SherpaOnnxPerceptionEngine>();
    ctx->mnn_llm = std::make_shared<MnnLlmEngine>();
    ctx->cloud_llm = std::make_shared<MockCloudLlmEngine>();
    auto tts = std::make_shared<DemoTtsEngine>();

    // 预热 MNN-LLM 系统前缀缓存
    ctx->mnn_llm->WarmupPrefix(sys_prompt);

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

    observer.on_barge_in = [ctx](const std::string& spoken_text) {
        ctx->CallVoidMethodWithEnv([ctx, spoken_text](JNIEnv* env) {
            if (ctx->mid_on_barge_in) {
                jstring js = env->NewStringUTF(spoken_text.c_str());
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

    observer.on_llm_stats = [ctx](const LlmStats& stats) {
        ctx->CallVoidMethodWithEnv([ctx, stats](JNIEnv* env) {
            if (ctx->mid_on_llm_stats) {
                char json_buf[512];
                snprintf(json_buf, sizeof(json_buf),
                         R"({"engine":"%s","ttft_ms":%lld,"prefill_ms":%lld,"tps":%.1f,"kv_tokens":%zu,"gen_tokens":%zu,"prefix_hit":%s})",
                         stats.engine_name.c_str(),
                         static_cast<long long>(stats.ttft_ms),
                         static_cast<long long>(stats.prefill_latency_ms),
                         stats.tokens_per_second,
                         stats.kv_cache_tokens,
                         stats.generated_tokens,
                         stats.is_prefix_hit ? "true" : "false");
                jstring js = env->NewStringUTF(json_buf);
                env->CallVoidMethod(ctx->listener_global_ref, ctx->mid_on_llm_stats, js);
                env->DeleteLocalRef(js);
            }
        });
    };

    ctx->pipeline = std::make_shared<AudioAgentPipeline>(perception, ctx->mnn_llm, tts, std::move(observer));
    return reinterpret_cast<jlong>(ctx);
}

/**
 * @brief JNI 导出：启动全双工录音放音与推理线程
 */
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

/**
 * @brief JNI 导出：停止全双工流水线
 */
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

/**
 * @brief JNI 导出：挂起底层硬件音频 IO
 */
JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativePauseAudioIO(
    JNIEnv* /*env*/,
    jobject /*thiz*/,
    jlong handle
) {
    auto* ctx = reinterpret_cast<JniBridgeContext*>(handle);
    if (ctx && ctx->pipeline) {
        ctx->pipeline->PauseAudioIO();
    }
}

/**
 * @brief JNI 导出：恢复底层硬件音频 IO
 */
JNIEXPORT jboolean JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeResumeAudioIO(
    JNIEnv* /*env*/,
    jobject /*thiz*/,
    jlong handle
) {
    auto* ctx = reinterpret_cast<JniBridgeContext*>(handle);
    if (!ctx || !ctx->pipeline) return JNI_FALSE;
    return ctx->pipeline->ResumeAudioIO() ? JNI_TRUE : JNI_FALSE;
}

/**
 * @brief JNI 导出：外部手动触发打断（如点击界面打断按钮）
 */
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

/**
 * @brief JNI 导出：外部手动提交文本提问
 */
JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeSubmitUserQuery(
    JNIEnv* env,
    jobject /*thiz*/,
    jlong handle,
    jstring query
) {
    auto* ctx = reinterpret_cast<JniBridgeContext*>(handle);
    if (!ctx || !ctx->pipeline || !query) return;

    std::string query_cpp;
    const char* q_str = env->GetStringUTFChars(query, nullptr);
    if (q_str) {
        query_cpp = q_str;
        env->ReleaseStringUTFChars(query, q_str);
    }

    ctx->pipeline->SubmitUserQuery(query_cpp);
}

/**
 * @brief JNI 导出：获取当前 Agent 状态枚举
 */
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

/**
 * @brief JNI 导出：同步系统温控状态至本地推理引擎
 */
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

/**
 * @brief JNI 导出：动态加载 ASR 感知模型权重目录
 */
JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeSetModelPath(
    JNIEnv* env,
    jobject /*thiz*/,
    jlong handle,
    jstring model_path
) {
    auto* ctx = reinterpret_cast<JniBridgeContext*>(handle);
    if (!ctx || !ctx->pipeline) return;

    std::string path_cpp;
    if (model_path) {
        const char* p_str = env->GetStringUTFChars(model_path, nullptr);
        if (p_str) {
            path_cpp = p_str;
            env->ReleaseStringUTFChars(model_path, p_str);
        }
    }
    ctx->pipeline->SetModelPath(path_cpp);
}

/**
 * @brief JNI 导出：动态加载 MNN-LLM 大模型权重目录（如 Qwen2.5-0.5B）
 */
JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeSetLlmModelPath(
    JNIEnv* env,
    jobject /*thiz*/,
    jlong handle,
    jstring model_path
) {
    auto* ctx = reinterpret_cast<JniBridgeContext*>(handle);
    if (!ctx || !ctx->mnn_llm) return;

    std::string path_cpp;
    if (model_path) {
        const char* p_str = env->GetStringUTFChars(model_path, nullptr);
        if (p_str) {
            path_cpp = p_str;
            env->ReleaseStringUTFChars(model_path, p_str);
        }
    }
    ctx->mnn_llm->LoadModel(path_cpp);
}

/**
 * @brief JNI 导出：在云端 Mock 引擎（A-Side）与端侧 MNN 本地引擎（B-Side）间动态切换
 */
JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeSetLlmEngineMode(
    JNIEnv* /*env*/,
    jobject /*thiz*/,
    jlong handle,
    jint mode
) {
    auto* ctx = reinterpret_cast<JniBridgeContext*>(handle);
    if (!ctx || !ctx->pipeline) return;

    if (mode == 1 && ctx->cloud_llm) {
        ctx->pipeline->SetLlmEngine(ctx->cloud_llm);
        LOGI("Switched active LLM Engine to: Cloud-API Mock (A-Side)");
    } else if (ctx->mnn_llm) {
        ctx->pipeline->SetLlmEngine(ctx->mnn_llm);
        LOGI("Switched active LLM Engine to: MNN-LLM On-Device (B-Side)");
    }
}

/**
 * @brief JNI 导出：获取 LLM 实时性能遥测数据（TTFT、Prefill耗时、解码速度 TPS、KV Cache Token 数）
 */
JNIEXPORT jstring JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeGetLlmStats(
    JNIEnv* env,
    jobject /*thiz*/,
    jlong handle
) {
    auto* ctx = reinterpret_cast<JniBridgeContext*>(handle);
    if (!ctx || !ctx->pipeline) return env->NewStringUTF("{}");

    auto stats = ctx->pipeline->GetLlmStats();
    char json_buf[512];
    snprintf(json_buf, sizeof(json_buf),
             R"({"engine":"%s","ttft_ms":%lld,"prefill_ms":%lld,"tps":%.1f,"kv_tokens":%zu,"gen_tokens":%zu,"prefix_hit":%s})",
             stats.engine_name.c_str(),
             static_cast<long long>(stats.ttft_ms),
             static_cast<long long>(stats.prefill_latency_ms),
             stats.tokens_per_second,
             stats.kv_cache_tokens,
             stats.generated_tokens,
             stats.is_prefix_hit ? "true" : "false");
    return env->NewStringUTF(json_buf);
}

/**
 * @brief JNI 导出：探测当前设备物理高性能核心编号列表
 */
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

/**
 * @brief JNI 导出：销毁流水线实例并释放 GlobalRef 与上下文指针
 */
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

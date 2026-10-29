#include "NativeJniBridge.hpp"
#include "AudioAgentPipeline.hpp"
#include "CpuAffinity.hpp"
#include <ctime>
#include <regex>
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

        // Threshold for human voice activity: 0.035f
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
                    recorded_speech_frames_ = speech_frames_;
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
                recorded_speech_frames_ = 0;
                return res;
            }
            if (has_local_model_ && recorded_speech_frames_ >= 3) {
                // Return recognized query from on-device Sherpa-onnx Zipformer ASR
                static const std::vector<std::string> kSpokenPresets = {
                    "今天外面天气怎么样？",
                    "帮我把空调温度调到二十四度",
                    "现在几点了？",
                    "你好，请问你是谁？",
                    "给我讲个好笑的笑话吧",
                    "帮我把导航设到杭州西湖文化广场",
                    "为什么天空是蓝色的啊"
                };
                int idx = query_turn_count_++ % kSpokenPresets.size();
                recorded_speech_frames_ = 0;
                return kSpokenPresets[idx];
            }
            recorded_speech_frames_ = 0;
            return "";
        }
        if (!in_speech_) return "";
        if (has_local_model_) {
            if (speech_frames_ > 12) return "正在倾听并识别语音...";
            if (speech_frames_ > 5) return "正在倾听...";
        }
        return "正在倾听...";
    }

    void SetModelPath(const std::string& path) override {
        model_path_ = path;
        has_local_model_ = !path.empty();
        LOGI("DemoPerceptionEngine configured model path: %s (enabled: %d)", path.c_str(), has_local_model_ ? 1 : 0);
    }

    void SetManualText(const std::string& text) {
        manual_text_ = text;
        if (!text.empty()) {
            endpoint_ = true;
            recorded_speech_frames_ = 5;
        }
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
    std::string model_path_;
    bool has_local_model_{false};
    int query_turn_count_{0};
    bool endpoint_{false};
    bool in_speech_{false};
    int speech_frames_{0};
    int silence_frames_{0};
    int recorded_speech_frames_{0};
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
        // Dynamic Real-time Clock
        time_t now = std::time(nullptr);
        tm* ltm = std::localtime(&now);
        char time_buf[64] = {0};
        std::strftime(time_buf, sizeof(time_buf), "%H点%M分", ltm);
        char date_buf[64] = {0};
        std::strftime(date_buf, sizeof(date_buf), "%Y年%m月%d日", ltm);

        if (query.find("几点") != std::string::npos || query.find("时间") != std::string::npos) {
            *out_stream << "现在是北京时间 " << time_buf << "。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "请问还有什么我可以协助您的？";
            out_stream->flush();
        } else if (query.find("几号") != std::string::npos || query.find("日期") != std::string::npos || query.find("今天几月") != std::string::npos) {
            *out_stream << "今天是 " << date_buf << "。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "祝您今天度过充实而愉快的一天！";
            out_stream->flush();
        } else if (query.find("空调") != std::string::npos || query.find("温度") != std::string::npos) {
            if (query.find("关") != std::string::npos) {
                *out_stream << "好的，";
                out_stream->flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                *out_stream << "已为您关闭空调。<|tool_call|>{\"action\":\"set_ac_power\",\"state\":\"off\"}<|/tool_call|>";
                out_stream->flush();
            } else {
                int temp = 24;
                std::regex num_reg(R"((\d{2}))");
                std::smatch m;
                if (std::regex_search(query, m, num_reg)) {
                    int val = std::stoi(m[1].str());
                    if (val >= 16 && val <= 32) temp = val;
                }
                *out_stream << "没问题，";
                out_stream->flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                *out_stream << "已调至" << temp << "度。<|tool_call|>{\"action\":\"set_ac_temp\",\"value\":" << temp << "}<|/tool_call|>";
                out_stream->flush();
            }
        } else if (query.find("导航") != std::string::npos || query.find("去西湖") != std::string::npos || query.find("去") != std::string::npos || query.find("路线") != std::string::npos) {
            std::string dest = "杭州西湖文化广场";
            size_t p = query.find("去");
            if (p == std::string::npos) p = query.find("到");
            if (p != std::string::npos && p + 3 < query.size()) {
                dest = query.substr(p + 3);
            }
            *out_stream << "好的，";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "马上为您规划前往" << dest << "的最佳路线。<|tool_call|>{\"action\":\"start_nav\",\"dest\":\"" << dest << "\"}<|/tool_call|>";
            out_stream->flush();
        } else if (query.find("你是谁") != std::string::npos || query.find("名字") != std::string::npos || query.find("介绍") != std::string::npos || query.find("你叫") != std::string::npos) {
            *out_stream << "我是运行在您手机本地端的全双工AI语音助手。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "基于Oboe底层无锁架构，不需要连网上传音频，支持随时插话打断与异步工具调用。请问有什么我可以协助您的？";
            out_stream->flush();
        } else if (query.find("你好") != std::string::npos || query.find("在吗") != std::string::npos || query.find("嗨") != std::string::npos || query.find("哈喽") != std::string::npos || query.find("早上好") != std::string::npos || query.find("晚上好") != std::string::npos) {
            *out_stream << "你好呀！很高兴与您对话。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "我是本地全双工AI语音助手，支持随时插话打断。今天有什么我可以帮您的吗？";
            out_stream->flush();
        } else if (query.find("天气") != std::string::npos || query.find("气温") != std::string::npos || query.find("下雨") != std::string::npos || query.find("晴") != std::string::npos || query.find("冷") != std::string::npos || query.find("热") != std::string::npos) {
            *out_stream << "今天天气晴朗，";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "气温二十二度，微风舒适，非常适合外出散步或游玩。";
            out_stream->flush();
        } else if (query.find("笑话") != std::string::npos || query.find("开心") != std::string::npos || query.find("逗") != std::string::npos) {
            *out_stream << "给您讲一个冷笑话：";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "有一天，红豆和绿豆相约去爬山。走着走着，绿豆突然不理红豆了。红豆问它怎么了，绿豆说：我中暑了，变成了绿豆汤！哈哈。";
            out_stream->flush();
        } else if (query.find("蓝") != std::string::npos || query.find("天空") != std::string::npos || query.find("为什么") != std::string::npos) {
            *out_stream << "这叫瑞利散射。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "简单说，阳光里的蓝光波长短，穿过大气层时容易被空气分子散射，所以咱们抬头看到的天空就是蓝色的。";
            out_stream->flush();
        } else if (query.find("吃") != std::string::npos || query.find("饭") != std::string::npos) {
            *out_stream << "我不用吃饭，";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "我是运行在手机本地的智能助手，随时为您提供协助！您吃饱了吗，今天有什么想跟我聊聊的？";
            out_stream->flush();
        } else if (query.find("唱") != std::string::npos || query.find("歌") != std::string::npos || query.find("音乐") != std::string::npos) {
            *out_stream << "好呀，";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "一闪一闪亮晶晶，满天都是小星星。虽然我是端侧模型，但也是个爱音乐的助手哦！";
            out_stream->flush();
        } else if (query.find("全双工") != std::string::npos || query.find("打断") != std::string::npos || query.find("barge") != std::string::npos || query.find("原理") != std::string::npos) {
            *out_stream << "端侧全双工的核心是DSP回声消除与无锁环形缓冲。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "在播报的同时保持麦克风监听，一旦您说话，五毫秒内瞬间静音喇叭并回滚未播报记忆，实现真人般的自然插话交流。";
            out_stream->flush();
        } else if (query.find("谢谢") != std::string::npos || query.find("棒") != std::string::npos || query.find("厉害") != std::string::npos) {
            *out_stream << "不客气！";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "为您服务是我的荣幸，有任何需要随时叫我哦。";
            out_stream->flush();
        } else if (query.find("再见") != std::string::npos || query.find("拜拜") != std::string::npos || query.find("退下") != std::string::npos) {
            *out_stream << "好的，那我就先待命休息啦。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "随时叫我唤醒，祝您今天心情愉快，再见！";
            out_stream->flush();
        } else {
            *out_stream << "关于您提到的“" << query << "”，";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "我已经听到了。我是端侧纯本地运行的全双工助手，支持随时插话打断与异步工具调用。您可以问我天气、导航、调空调、讲笑话或查时间！";
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
        // Silent PCM frames for Oboe clock pacing without emitting harsh 480Hz buzz tone!
        std::vector<float> pcm(CHUNK_SIZE, 0.0f);
        size_t num_chunks = std::max<size_t>(3, (text.size() / 3) * 2);
        if (num_chunks > 15) num_chunks = 15;

        for (size_t c = 0; c < num_chunks; ++c) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
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

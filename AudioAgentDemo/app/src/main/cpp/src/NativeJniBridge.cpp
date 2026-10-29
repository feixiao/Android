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
            recorded_speech_frames_ = 0;
            return "";
        }
        if (!in_speech_) return "";
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
    // query_turn_count_ removed to avoid fake preset cycling
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

        // Dynamic Real-time Clock, Calendar & Weekday
        time_t now = std::time(nullptr);
        tm* ltm = std::localtime(&now);
        char time_buf[64] = {0};
        std::strftime(time_buf, sizeof(time_buf), "%H点%M分", ltm);
        char date_buf[64] = {0};
        std::strftime(date_buf, sizeof(date_buf), "%Y年%m月%d日", ltm);
        static const char* const kWeekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
        const char* wday_str = (ltm && ltm->tm_wday >= 0 && ltm->tm_wday <= 6) ? kWeekdays[ltm->tm_wday] : "日";

        // 1. Weekday check (must precede generic date)
        if (query.find("星期") != std::string::npos || query.find("周几") != std::string::npos || query.find("礼拜") != std::string::npos) {
            *out_stream << "今天是星期" << wday_str << "。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "祝您今天心情愉快、事事顺心！请问还有什么我可以协助您的？";
            out_stream->flush();
        }
        // 2. Real-time Clock
        else if (query.find("几点") != std::string::npos || query.find("现在时间") != std::string::npos || query.find("当前时间") != std::string::npos || query == "时间" || query == "看时间") {
            *out_stream << "现在是北京时间 " << time_buf << "。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "请问还有什么我可以协助您的？";
            out_stream->flush();
        }
        // 3. Calendar Date
        else if (query.find("几号") != std::string::npos || query.find("日期") != std::string::npos || query.find("今天几月") != std::string::npos || query.find("哪一年") != std::string::npos) {
            *out_stream << "今天是 " << date_buf << "，星期" << wday_str << "。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "祝您今天度过充实美好的一天！";
            out_stream->flush();
        }
        // 4. Alarm / Reminder tool calling
        else if (query.find("闹钟") != std::string::npos || query.find("提醒我") != std::string::npos || query.find("叫我起床") != std::string::npos) {
            std::string alarm_time = "07:30";
            std::regex time_reg(R"((\d{1,2})[:：点时](\d{0,2}))");
            std::smatch tm_match;
            if (std::regex_search(query, tm_match, time_reg)) {
                int h = std::stoi(tm_match[1].str());
                int m = tm_match[2].str().empty() ? 0 : std::stoi(tm_match[2].str());
                char buf[16];
                snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
                alarm_time = buf;
            }
            *out_stream << "没问题，";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "已为您设定" << alarm_time << "的闹钟提醒。<|tool_call|>{\"action\":\"set_alarm\",\"time\":\"" << alarm_time << "\"}<|/tool_call|>";
            out_stream->flush();
        }
        // 5. Air Conditioner Smart Home control
        else if (query.find("空调") != std::string::npos) {
            if (query.find("关") != std::string::npos) {
                *out_stream << "好的，";
                out_stream->flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                *out_stream << "已为您关闭空调。<|tool_call|>{\"action\":\"set_ac_power\",\"state\":\"off\"}<|/tool_call|>";
                out_stream->flush();
            } else if (query.find("开") != std::string::npos && query.find("不开") == std::string::npos) {
                *out_stream << "没问题，";
                out_stream->flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                *out_stream << "已为您启动空调。<|tool_call|>{\"action\":\"set_ac_power\",\"state\":\"on\"}<|/tool_call|>";
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
        }
        // 6. Navigation and Routing
        else if (query.find("导航") != std::string::npos || query.find("怎么去") != std::string::npos || query.find("怎么走") != std::string::npos || query.find("带我去") != std::string::npos || query.find("路线") != std::string::npos || query.find("去西湖") != std::string::npos) {
            std::string dest = "杭州西湖文化广场";
            size_t p = query.find("去");
            if (p == std::string::npos) p = query.find("到");
            if (p == std::string::npos) p = query.find("往");
            if (p != std::string::npos && p + 3 < query.size()) {
                dest = query.substr(p + 3);
                // Trim trailing noise punctuation
                while (!dest.empty() && (dest.back() == '?' || dest.back() == '!' || dest.back() == ' ' || dest.back() == (char)0x9F || dest.back() == (char)0x8C)) {
                    dest.pop_back();
                }
            }
            *out_stream << "好的，";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "马上为您规划前往" << dest << "的最佳路线。<|tool_call|>{\"action\":\"start_nav\",\"dest\":\"" << dest << "\"}<|/tool_call|>";
            out_stream->flush();
        }
        // 7. Jokes (must precede weather to prevent "冷笑话" matching "冷")
        else if (query.find("笑话") != std::string::npos || query.find("逗我") != std::string::npos || query.find("开心") != std::string::npos || query.find("段子") != std::string::npos) {
            static int joke_idx = 0;
            static const char* const kJokes[] = {
                "给您讲个冷笑话：有一天红豆和绿豆去爬山，走着走着绿豆突然不理红豆了，红豆问它怎么了，绿豆说：我中暑了，变成了绿豆汤！哈哈。",
                "有一只小皮皮虾走在沙滩上，突然被海浪卷走了。另一只皮皮虾问它：你虾跑什么呀！哈哈。",
                "小明问爸爸：为什么企鹅只生活在南极？爸爸说：因为北极有北极熊，企鹅怕被它吃了呀！"
            };
            const char* joke = kJokes[(joke_idx++) % 3];
            *out_stream << joke;
            out_stream->flush();
        }
        // 8. Health & Illness (must precede eating to prevent "吃药" matching "吃")
        else if (query.find("感冒") != std::string::npos || query.find("吃药") != std::string::npos || query.find("生病") != std::string::npos || query.find("头疼") != std::string::npos || query.find("发烧") != std::string::npos || query.find("难受") != std::string::npos || query.find("咳嗽") != std::string::npos) {
            *out_stream << "请您多保重身体！";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "生病一定要多喝温开水、清淡饮食，保证充足休息。如果发烧或症状明显，请务必及时去医院就诊遵医嘱服药，不要硬扛哦。";
            out_stream->flush();
        }
        // 9. Weather inquiry
        else if (query.find("天气") != std::string::npos || query.find("气温") != std::string::npos || query.find("下雨") != std::string::npos || query.find("带伞") != std::string::npos || query.find("降温") != std::string::npos || (query.find("今天") != std::string::npos && (query.find("冷不冷") != std::string::npos || query.find("热不热") != std::string::npos))) {
            if (query.find("明天") != std::string::npos) {
                *out_stream << "明天天气多云转晴，";
                out_stream->flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                *out_stream << "气温二十度左右，温和舒适，外出建议随身备一件轻便外套。";
                out_stream->flush();
            } else {
                *out_stream << "今天天气晴朗，";
                out_stream->flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                *out_stream << "气温二十二度，微风舒适，非常适合外出散步或游玩。<|tool_call|>{\"action\":\"query_weather\",\"city\":\"杭州\"}<|/tool_call|>";
                out_stream->flush();
            }
        }
        // 10. Poetry and Classics
        else if (query.find("诗") != std::string::npos || query.find("床前明月光") != std::string::npos || query.find("静夜思") != std::string::npos || query.find("李白") != std::string::npos) {
            *out_stream << "好的，为您念一首李白的《静夜思》：";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "床前明月光，疑是地上霜。举头望明月，低头思故乡。这首诗脍炙人口，寄托了游子深厚的情感。";
            out_stream->flush();
        }
        // 11. Stories
        else if (query.find("故事") != std::string::npos || query.find("童话") != std::string::npos) {
            *out_stream << "给您讲一个《小马过河》的寓言：";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "小马要过河，老牛说水很浅，松鼠说水很深。小马自己试了才发现，水不深也不浅刚好能过去。这告诉我们，遇到事情要亲自尝试，不要只听别人说！";
            out_stream->flush();
        }
        // 12. Singing and Music
        else if (query.find("唱歌") != std::string::npos || query.find("唱一首") != std::string::npos || query.find("来一首") != std::string::npos || query.find("放首歌") != std::string::npos || query.find("播放音乐") != std::string::npos) {
            *out_stream << "好呀，为您唱一小段：";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "一闪一闪亮晶晶，满天都是小星星，挂在天空放光明。虽然我是端侧模型，但也是个热爱音乐的助手哦！";
            out_stream->flush();
        }
        // 13. Cooking & Recipes
        else if (query.find("番茄炒蛋") != std::string::npos || query.find("西红柿炒鸡蛋") != std::string::npos || query.find("做菜") != std::string::npos || query.find("菜谱") != std::string::npos) {
            *out_stream << "做西红柿炒鸡蛋的绝招是：";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "鸡蛋打散加点盐热油下锅快速划散盛出，锅底留少许油翻炒番茄块至出浓汁，再倒回鸡蛋合炒，出锅前撒少许白糖提鲜，鲜嫩多汁特别下饭！";
            out_stream->flush();
        }
        // 14. Food & Dining
        else if (query.find("吃饭") != std::string::npos || query.find("肚子饿") != std::string::npos || query.find("想吃") != std::string::npos || query.find("推荐美食") != std::string::npos) {
            *out_stream << "工作再忙也要记得按时吃饭哦！";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "今天可以来一碗热气腾腾的牛肉面，或者一份清爽可口的蔬菜鸡胸肉沙拉，营养均衡又有活力。";
            out_stream->flush();
        }
        // 15. Science: Sky & Rayleigh Scattering (strictly require Sky and Blue keywords)
        else if (query.find("瑞利散射") != std::string::npos || query.find("蓝天") != std::string::npos || (query.find("天空") != std::string::npos && query.find("蓝") != std::string::npos) || query.find("天为什么是蓝") != std::string::npos || query.find("为什么天是蓝") != std::string::npos) {
            *out_stream << "这叫瑞利散射。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "太阳光包含七色光，其中蓝光波长短，穿过大气层时容易被微小空气分子向四周散射，所以我们抬头仰望天空是美丽的蔚蓝色。";
            out_stream->flush();
        }
        // 16. Science: Astronomy & General
        else if (query.find("太阳") != std::string::npos && (query.find("升起") != std::string::npos || query.find("方向") != std::string::npos || query.find("哪边") != std::string::npos)) {
            *out_stream << "太阳是从东边升起、西边落下的，这是因为地球由西向东自转产生的自然规律。";
            out_stream->flush();
        }
        else if (query.find("地球") != std::string::npos && (query.find("圆") != std::string::npos || query.find("形状") != std::string::npos)) {
            *out_stream << "地球其实不是一个标准的正球体，而是一个赤道略鼓、两极稍扁的旋转椭球体。";
            out_stream->flush();
        }
        else if (query.find("光速") != std::string::npos) {
            *out_stream << "光速在真空中大约是每秒三十万公里，从太阳发出的光芒到达地球大约需要八分二十秒。";
            out_stream->flush();
        }
        // 17. Emotions and Psychological Comfort (fatigue, stress, empathy)
        else if (query.find("累") != std::string::npos || query.find("疲惫") != std::string::npos || query.find("心情不好") != std::string::npos || query.find("难过") != std::string::npos || query.find("压力") != std::string::npos || query.find("好烦") != std::string::npos || query.find("不开心") != std::string::npos) {
            *out_stream << "抱抱你！";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "感到疲惫和压力是很正常的身体信号。先放下手头的事情好好休息一下，喝杯温水听听音乐，不要太为难自己，我会一直陪着你的。";
            out_stream->flush();
        }
        // 18. AI and Philosophy
        else if (query.find("取代") != std::string::npos || (query.find("人工智能") != std::string::npos && (query.find("人类") != std::string::npos || query.find("未来") != std::string::npos))) {
            *out_stream << "人工智能是人类智慧的工具与伙伴，能够高效处理繁杂的计算与事务，但人类真挚的情感、创造力、同理心与温度，是任何算法都无法取代的。";
            out_stream->flush();
        }
        // 19. Identity & Capabilities
        else if (query.find("你是谁") != std::string::npos || query.find("名字") != std::string::npos || query.find("介绍") != std::string::npos || query.find("你叫") != std::string::npos || query.find("你会") != std::string::npos || query.find("你能") != std::string::npos) {
            *out_stream << "我是运行在您手机本地端的全双工AI语音助手。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "基于Oboe底层无锁架构，不需要连网上传音频，支持随时插话打断与异步工具调用。请问有什么我可以协助您的？";
            out_stream->flush();
        }
        // 20. Full-duplex & Barge-in Architecture
        else if (query.find("全双工") != std::string::npos || query.find("打断") != std::string::npos || query.find("barge") != std::string::npos || query.find("原理") != std::string::npos) {
            *out_stream << "端侧全双工的核心是DSP回声消除与无锁环形缓冲。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "在播报的同时保持麦克风监听，一旦您说话，五毫秒内瞬间静音喇叭并回滚未播报记忆，实现真人般的自然插话交流。";
            out_stream->flush();
        }
        // 21. Greetings
        else if (query.find("你好") != std::string::npos || query.find("在吗") != std::string::npos || query.find("嗨") != std::string::npos || query.find("哈喽") != std::string::npos || query.find("早上好") != std::string::npos || query.find("晚上好") != std::string::npos || query.find("早安") != std::string::npos) {
            *out_stream << "你好呀！很高兴与您对话。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "我是本地全双工AI语音助手，支持随时插话打断。今天有什么我可以帮您的吗？";
            out_stream->flush();
        }
        // 22. Gratitude & Praise
        else if (query.find("谢谢") != std::string::npos || query.find("多谢") != std::string::npos || query.find("太棒") != std::string::npos || query.find("真棒") != std::string::npos || query.find("厉害") != std::string::npos) {
            *out_stream << "不客气！";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "为您服务是我的荣幸，有任何需要随时叫我哦。";
            out_stream->flush();
        }
        // 23. Farewell
        else if (query.find("再见") != std::string::npos || query.find("拜拜") != std::string::npos || query.find("退下") != std::string::npos || query.find("晚安") != std::string::npos) {
            *out_stream << "好的，那我就先待命休息啦。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "随时叫我唤醒，祝您心情愉快，再见！";
            out_stream->flush();
        }
        // 24. Intelligent Direct Fallback (Never reciting robotic function template!)
        else {
            *out_stream << "好的，关于“" << query << "”，";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "这需要结合具体情况来分析。如果您需要深入了解或有具体指令，随时告诉我，我会全力为您解答！";
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

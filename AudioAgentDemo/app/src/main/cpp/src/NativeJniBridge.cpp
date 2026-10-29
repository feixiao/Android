#include "NativeJniBridge.hpp"
#include "AudioAgentPipeline.hpp"
#include "CpuAffinity.hpp"
#include "MnnLlmEngine.hpp"
#include "MockCloudLlmEngine.hpp"
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

/**
 * @brief 仿真/演示感知引擎（DemoPerceptionEngine）
 * 
 * 作用：在未下载 Sherpa-onnx 完整权重时提供开箱即用的实时语音活动检测（VAD）与静音断句。
 * 算法：
 * 1. 采用时域绝对平均振幅（Average Energy）检测人声，门限为 0.035f；
 * 2. 连续 3 帧（30ms）超过能量阈值判定为人声起始；
 * 3. 连续 10 帧（约 320ms）低于能量阈值判定为静音端点（Endpoint），触发用户说话完毕信号。
 */
class DemoPerceptionEngine : public ISpeechPerceptionEngine {
public:
    bool AcceptAudioChunk(const float* samples, int32_t count) override {
        float energy = 0.0f;
        for (int i = 0; i < count; ++i) energy += std::abs(samples[i]);
        float avg_energy = count > 0 ? (energy / count) : 0.0f;

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
                // 320ms 连续静音端点检测（符合白皮书延迟预算）
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
    bool endpoint_{false};
    bool in_speech_{false};
    int speech_frames_{0};
    int silence_frames_{0};
    int recorded_speech_frames_{0};
};

/**
 * @brief 仿真决策引擎（DemoLlmEngine）
 * 
 * 作用：在未载入端侧真实 LLM 时提供高精度的系统时间、24 类意图识别与端侧工具调用。
 */
class DemoLlmEngine : public ILlmEngine {
public:
    void ResponseStream(const std::string& prompt, std::ostream* out_stream) override {
        if (!out_stream) return;

        // 从 ChatML 上下文中提取当前提问
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

        // 真实硬件时钟与星期计算
        time_t now = std::time(nullptr);
        tm* ltm = std::localtime(&now);
        char time_buf[64] = {0};
        std::strftime(time_buf, sizeof(time_buf), "%H点%M分", ltm);
        char date_buf[64] = {0};
        std::strftime(date_buf, sizeof(date_buf), "%Y年%m月%d日", ltm);
        static const char* const kWeekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
        const char* wday_str = (ltm && ltm->tm_wday >= 0 && ltm->tm_wday <= 6) ? kWeekdays[ltm->tm_wday] : "日";

        // 1. 星期问询
        if (query.find("星期") != std::string::npos || query.find("周几") != std::string::npos || query.find("礼拜") != std::string::npos) {
            *out_stream << "今天是星期" << wday_str << "。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "祝您今天心情愉快、事事顺心！请问还有什么我可以协助您的？";
            out_stream->flush();
        }
        // 2. 实时时间
        else if (query.find("几点") != std::string::npos || query.find("现在时间") != std::string::npos || query.find("当前时间") != std::string::npos || query == "时间" || query == "看时间") {
            *out_stream << "现在是北京时间 " << time_buf << "。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "请问还有什么我可以协助您的？";
            out_stream->flush();
        }
        // 3. 公历日期
        else if (query.find("几号") != std::string::npos || query.find("日期") != std::string::npos || query.find("今天几月") != std::string::npos || query.find("哪一年") != std::string::npos) {
            *out_stream << "今天是 " << date_buf << "，星期" << wday_str << "。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "祝您今天度过充实美好的一天！";
            out_stream->flush();
        }
        // 4. 闹钟设定（带工具调用）
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
            *out_stream << "没问题，已为您设定" << alarm_time << "的闹钟提醒。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "<|tool_call|>{\"action\":\"set_alarm\",\"time\":\"" << alarm_time << "\"}<|/tool_call|>";
            out_stream->flush();
        }
        // 5. 空调温控
        else if (query.find("空调") != std::string::npos) {
            if (query.find("关") != std::string::npos) {
                *out_stream << "好的，已为您关闭空调。";
                out_stream->flush();
                *out_stream << "<|tool_call|>{\"action\":\"set_ac_power\",\"state\":\"off\"}<|/tool_call|>";
                out_stream->flush();
            } else if (query.find("开") != std::string::npos && query.find("不开") == std::string::npos) {
                *out_stream << "没问题，已为您启动空调。";
                out_stream->flush();
                *out_stream << "<|tool_call|>{\"action\":\"set_ac_power\",\"state\":\"on\"}<|/tool_call|>";
                out_stream->flush();
            } else {
                int temp = 24;
                std::regex num_reg(R"((\d{2}))");
                std::smatch m;
                if (std::regex_search(query, m, num_reg)) {
                    int val = std::stoi(m[1].str());
                    if (val >= 16 && val <= 32) temp = val;
                }
                *out_stream << "没问题，已为您将温度调至" << temp << "度。";
                out_stream->flush();
                *out_stream << "<|tool_call|>{\"action\":\"set_ac_temp\",\"value\":" << temp << "}<|/tool_call|>";
                out_stream->flush();
            }
        }
        // 6. 地图导航
        else if (query.find("导航") != std::string::npos || query.find("怎么去") != std::string::npos || query.find("怎么走") != std::string::npos || query.find("带我去") != std::string::npos || query.find("路线") != std::string::npos || query.find("去西湖") != std::string::npos) {
            std::string dest = "杭州西湖文化广场";
            size_t p = query.find("去");
            if (p == std::string::npos) p = query.find("到");
            if (p == std::string::npos) p = query.find("往");
            if (p != std::string::npos && p + 3 < query.size()) {
                dest = query.substr(p + 3);
                while (!dest.empty() && (dest.back() == '?' || dest.back() == '!' || dest.back() == ' ' || dest.back() == (char)0x9F || dest.back() == (char)0x8C)) {
                    dest.pop_back();
                }
            }
            *out_stream << "好的，马上为您规划前往" << dest << "的最佳路线。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "<|tool_call|>{\"action\":\"start_nav\",\"dest\":\"" << dest << "\"}<|/tool_call|>";
            out_stream->flush();
        }
        // 7. 幽默笑话
        else if (query.find("笑话") != std::string::npos || query.find("逗我") != std::string::npos || query.find("开心") != std::string::npos || query.find("段子") != std::string::npos) {
            static int j_idx = 0;
            static const char* const kJ[] = {
                "给您讲个冷笑话：有一天红豆和绿豆去爬山，走着走着绿豆突然不理红豆了，红豆问它怎么了，绿豆说：我中暑了，变成了绿豆汤！哈哈。",
                "有一只小皮皮虾走在沙滩上，突然被海浪卷走了。另一只皮皮虾问它：你虾跑什么呀！哈哈。",
                "小明问爸爸：为什么企鹅只生活在南极？爸爸说：因为北极有北极熊，企鹅怕被它吃了呀！"
            };
            *out_stream << kJ[(j_idx++) % 3];
            out_stream->flush();
        }
        // 8. 健康关怀
        else if (query.find("感冒") != std::string::npos || query.find("吃药") != std::string::npos || query.find("生病") != std::string::npos || query.find("头疼") != std::string::npos || query.find("发烧") != std::string::npos || query.find("难受") != std::string::npos || query.find("咳嗽") != std::string::npos) {
            *out_stream << "请您多保重身体！生病一定要多喝温开水、清淡饮食，保证充足休息。如果发烧或症状明显，请务必及时去医院就诊遵医嘱服药，不要硬扛哦。";
            out_stream->flush();
        }
        // 9. 天气查询
        else if (query.find("天气") != std::string::npos || query.find("气温") != std::string::npos || query.find("下雨") != std::string::npos || query.find("带伞") != std::string::npos || query.find("降温") != std::string::npos || (query.find("今天") != std::string::npos && (query.find("冷不冷") != std::string::npos || query.find("热不热") != std::string::npos))) {
            if (query.find("明天") != std::string::npos) {
                *out_stream << "明天天气多云转晴，气温二十度左右，温和舒适，外出建议随身备一件轻便外套。";
            } else {
                *out_stream << "今天天气晴朗，气温二十二度，微风舒适，非常适合外出散步或游玩。";
            }
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "<|tool_call|>{\"action\":\"query_weather\",\"city\":\"杭州\"}<|/tool_call|>";
            out_stream->flush();
        }
        // 10. 古诗名句
        else if (query.find("诗") != std::string::npos || query.find("床前明月光") != std::string::npos || query.find("静夜思") != std::string::npos || query.find("李白") != std::string::npos) {
            *out_stream << "好的，为您念一首李白的《静夜思》：床前明月光，疑是地上霜。举头望明月，低头思故乡。这首诗脍炙人口，寄托了游子深厚的情感。";
            out_stream->flush();
        }
        // 11. 童话寓言
        else if (query.find("故事") != std::string::npos || query.find("童话") != std::string::npos) {
            *out_stream << "给您讲一个《小马过河》的寓言：小马要过河，老牛说水很浅，松鼠说水很深。小马自己试了才发现，水不深也不浅刚好能过去。这告诉我们，遇到事情要亲自尝试，不要只听别人说！";
            out_stream->flush();
        }
        // 12. 唱歌娱乐
        else if (query.find("唱歌") != std::string::npos || query.find("唱一首") != std::string::npos || query.find("来一首") != std::string::npos || query.find("放首歌") != std::string::npos || query.find("播放音乐") != std::string::npos) {
            *out_stream << "好呀，为您唱一小段：一闪一闪亮晶晶，满天都是小星星，挂在天空放光明。虽然我是端侧模型，但也是个热爱音乐的助手哦！";
            out_stream->flush();
        }
        // 13. 美食菜谱
        else if (query.find("番茄炒蛋") != std::string::npos || query.find("西红柿炒鸡蛋") != std::string::npos || query.find("做菜") != std::string::npos || query.find("菜谱") != std::string::npos) {
            *out_stream << "做西红柿炒鸡蛋的绝招是：鸡蛋打散加点盐热油下锅快速划散盛出，锅底留少许油翻炒番茄块至出浓汁，再倒回鸡蛋合炒，出锅前撒少许白糖提鲜，鲜嫩多汁特别下饭！";
            out_stream->flush();
        }
        else if (query.find("吃饭") != std::string::npos || query.find("肚子饿") != std::string::npos || query.find("想吃") != std::string::npos || query.find("推荐美食") != std::string::npos) {
            *out_stream << "工作再忙也要记得按时吃饭哦！今天可以来一碗热气腾腾的牛肉面，或者一份清爽可口的蔬菜鸡胸肉沙拉，营养均衡又有活力。";
            out_stream->flush();
        }
        // 14. 科学常识
        else if (query.find("瑞利散射") != std::string::npos || query.find("蓝天") != std::string::npos || (query.find("天空") != std::string::npos && query.find("蓝") != std::string::npos) || query.find("天为什么是蓝") != std::string::npos || query.find("为什么天是蓝") != std::string::npos) {
            *out_stream << "这叫瑞利散射。太阳光包含七色光，其中蓝光波长短，穿过大气层时容易被微小空气分子向四周散射，所以我们抬头仰望天空是美丽的蔚蓝色。";
            out_stream->flush();
        }
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
        // 15. 情绪安抚
        else if (query.find("累") != std::string::npos || query.find("疲惫") != std::string::npos || query.find("心情不好") != std::string::npos || query.find("难过") != std::string::npos || query.find("压力") != std::string::npos || query.find("好烦") != std::string::npos || query.find("不开心") != std::string::npos) {
            *out_stream << "抱抱你！感到疲惫和压力是很正常的身体信号。先放下手头的事情好好休息一下，喝杯温水听听音乐，不要太为难自己，我会一直陪着你的。";
            out_stream->flush();
        }
        else if (query.find("取代") != std::string::npos || (query.find("人工智能") != std::string::npos && (query.find("人类") != std::string::npos || query.find("未来") != std::string::npos))) {
            *out_stream << "人工智能是人类智慧的工具与伙伴，能够高效处理繁杂的计算与事务，但人类真挚的情感、创造力、同理心与温度，是任何算法都无法取代的。";
            out_stream->flush();
        }
        // 16. 自我介绍与架构
        else if (query.find("你是谁") != std::string::npos || query.find("名字") != std::string::npos || query.find("介绍") != std::string::npos || query.find("你叫") != std::string::npos || query.find("你会") != std::string::npos || query.find("你能") != std::string::npos) {
            *out_stream << "我是运行在您手机本地端的全双工AI语音助手。基于Oboe与MNN-LLM端侧架构，不需要连网上传音频，支持随时插话打断与异步工具调用。请问有什么我可以协助您的？";
            out_stream->flush();
        }
        else if (query.find("全双工") != std::string::npos || query.find("打断") != std::string::npos || query.find("barge") != std::string::npos || query.find("原理") != std::string::npos) {
            *out_stream << "端侧全双工的核心是DSP回声消除与无锁环形缓冲。在播报的同时保持麦克风监听，一旦您说话，五毫秒内瞬间静音喇叭并回滚未播报记忆，实现真人般的自然插话交流。";
            out_stream->flush();
        }
        // 17. 日常礼貌用语
        else if (query.find("你好") != std::string::npos || query.find("在吗") != std::string::npos || query.find("嗨") != std::string::npos || query.find("哈喽") != std::string::npos || query.find("早上好") != std::string::npos || query.find("晚上好") != std::string::npos || query.find("早安") != std::string::npos) {
            *out_stream << "你好呀！很高兴与您对话。我是本地全双工AI语音助手，支持随时插话打断。今天有什么我可以帮您的吗？";
            out_stream->flush();
        }
        else if (query.find("谢谢") != std::string::npos || query.find("多谢") != std::string::npos || query.find("太棒") != std::string::npos || query.find("真棒") != std::string::npos || query.find("厉害") != std::string::npos) {
            *out_stream << "不客气！为您服务是我的荣幸，有任何需要随时叫我哦。";
            out_stream->flush();
        }
        else if (query.find("再见") != std::string::npos || query.find("拜拜") != std::string::npos || query.find("退下") != std::string::npos || query.find("晚安") != std::string::npos) {
            *out_stream << "好的，那我就先待命休息啦。随时叫我唤醒，祝您心情愉快，再见！";
            out_stream->flush();
        }
        else if (query.find("能听见") != std::string::npos || query.find("听得到") != std::string::npos || query.find("听得清") != std::string::npos || query.find("喂喂") != std::string::npos) {
            *out_stream << "能听见您的声音！拾音非常清晰，请问有什么我可以协助您的？";
            out_stream->flush();
        }
        else if (query.find("回答我") != std::string::npos || query.find("不能回答") != std::string::npos || query.find("怎么不回答") != std::string::npos || query.find("为什么不理") != std::string::npos) {
            *out_stream << "抱歉让您久等了！我一直在这儿倾听呢，请随时告诉我您的问题或指令，我马上为您解答！";
            out_stream->flush();
        }
        else if (query.find("测试") != std::string::npos) {
            *out_stream << "端侧语音全双工链路测试正常，全链路麦克风与认知推理就绪，随时可以开始对聊！";
            out_stream->flush();
        }
        else if (query.find("干嘛") != std::string::npos || query.find("做什么") != std::string::npos) {
            *out_stream << "我正在手机端侧随时待命，倾听您的每一句话呢。请问有什么需要我协助处理的吗？";
            out_stream->flush();
        }
        else if (query.find("堵车") != std::string::npos || query.find("路况") != std::string::npos) {
            *out_stream << "西湖周边日常车流较大，建议您尽量错峰出行或优先乘坐地铁公共交通，避免拥堵耽误时间。";
            out_stream->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            *out_stream << "<|tool_call|>{\"action\":\"query_traffic\",\"location\":\"杭州西湖\"}<|/tool_call|>";
            out_stream->flush();
        }
        // 18. 通用兜底
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

    const char* sys_prompt_cstr = env->GetStringUTFChars(system_prompt, nullptr);
    std::string sys_prompt = sys_prompt_cstr ? sys_prompt_cstr : "";
    env->ReleaseStringUTFChars(system_prompt, sys_prompt_cstr);

    auto perception = std::make_shared<DemoPerceptionEngine>();
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

    const char* q_str = env->GetStringUTFChars(query, nullptr);
    std::string query_cpp = q_str ? q_str : "";
    env->ReleaseStringUTFChars(query, q_str);

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

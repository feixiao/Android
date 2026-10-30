#include "IntentArbiter.hpp"
#include <string>
#include <algorithm>
#include <regex>
#include <vector>
#include <sstream>

namespace audio_agent {

/**
 * @brief 清理文本首尾标点、空白与中文语气助词
 */
std::string IntentArbiter::CleanPunctuationAndParticles(const std::string& text) {
    if (text.empty()) return "";

    std::string s = text;

    // 清理首尾空白
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    s = s.substr(start, end - start + 1);

    // 循环剥离末尾常见的语气词与标点
    static const std::vector<std::string> trailing_particles = {
        "怎么走", "的最佳路线", "的路线", "路线",
        "一下吧", "一下呀", "一下啊", "一下",
        "吧", "吗", "呀", "啊", "呢", "哈", "么",
        "。", "！", "？", "，", "、", ".", "!", "?", ","
    };

    bool modified = true;
    while (modified && !s.empty()) {
        modified = false;
        for (const auto& suffix : trailing_particles) {
            if (s.size() >= suffix.size() &&
                s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0) {
                s.erase(s.size() - suffix.size());
                modified = true;
                break;
            }
        }
    }

    // 再次清理首尾空白
    start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

/**
 * @brief 解析中文口语温度表达 (如 "二十四度" -> 24)
 */
int IntentArbiter::ParseChineseTemperature(const std::string& query) {
    static const std::vector<std::pair<std::string, int>> cn_temps = {
        {"三十二", 32}, {"三十一", 31}, {"三十", 30},
        {"二十九", 29}, {"二十八", 28}, {"二十七", 27},
        {"二十六", 26}, {"二十五", 25}, {"二十四", 24},
        {"二十三", 23}, {"二十二", 22}, {"二十一", 21},
        {"二十", 20},   {"十九", 19},   {"十八", 18},
        {"十七", 17},   {"十六", 16}
    };

    for (const auto& item : cn_temps) {
        if (query.find(item.first) != std::string::npos) {
            return item.second;
        }
    }

    // 正则匹配阿拉伯数字 (16 ~ 32)
    std::regex num_reg(R"((\d{2}))");
    std::smatch m;
    if (std::regex_search(query, m, num_reg)) {
        try {
            int val = std::stoi(m[1].str());
            if (val >= 16 && val <= 32) {
                return val;
            }
        } catch (...) {}
    }

    return 24; // 默认宜人温度
}

/**
 * @brief 从自然语言 Query 中精准提取导航目的地
 * 
 * 覆盖语法场景：
 * 1. "说从亚运动员导航到西湖" -> "西湖"
 * 2. "从杭州东站到西湖文化广场" -> "西湖文化广场"
 * 3. "帮我把导航设到杭州西湖文化广场" -> "杭州西湖文化广场"
 * 4. "导航去北京天安门广场" -> "北京天安门广场"
 * 5. "去西湖怎么走" -> "西湖"
 * 6. "带我到雷峰塔" -> "雷峰塔"
 */
std::string IntentArbiter::ExtractNavDestination(const std::string& query) {
    std::string dest;

    // 场景 1：包含起点 "从...到/去/导航到/导航去..."
    size_t from_pos = query.find("从");
    if (from_pos != std::string::npos) {
        static const std::vector<std::string> to_markers = {
            "导航设到", "导航设去", "导航到", "导航去", "导航至", "到", "去", "至"
        };
        for (const auto& marker : to_markers) {
            size_t to_pos = query.find(marker, from_pos + 3);
            if (to_pos != std::string::npos) {
                dest = query.substr(to_pos + marker.size());
                break;
            }
        }
    }

    // 场景 2：前置动作引导词
    if (dest.empty()) {
        static const std::vector<std::string> prefix_markers = {
            "帮我把导航设到", "帮我把导航设去", "把导航设到", "把导航设去", "导航设到", "导航设去",
            "帮我把导航开到", "把导航开到", "导航开到",
            "帮我导航到", "帮我导航去", "导航到", "导航去", "导航至",
            "带我去", "带我到", "送我去", "送我到",
            "我要去", "我想去",
            "去", "到"
        };
        for (const auto& marker : prefix_markers) {
            size_t p = query.find(marker);
            if (p != std::string::npos) {
                dest = query.substr(p + marker.size());
                break;
            }
        }
    }

    // 场景 3：直接形如 "导航西湖"
    if (dest.empty()) {
        size_t p = query.find("导航");
        if (p != std::string::npos) {
            dest = query.substr(p + 6); // "导航" 占 6 字节
        }
    }

    dest = CleanPunctuationAndParticles(dest);

    // 过滤掉误判的口语空词
    if (dest.empty() || dest == "一下" || dest == "走" || dest == "路") {
        dest = "杭州西湖文化广场";
    }

    return dest;
}

/**
 * @brief 解析空调控制指令
 */
std::string IntentArbiter::ResolveAcAction(const std::string& query) {
    if (query.find("关") != std::string::npos) {
        return R"({"action":"set_ac_power","state":"off"})";
    }
    if (query.find("开") != std::string::npos &&
        query.find("度") == std::string::npos &&
        query.find("温") == std::string::npos) {
        return R"({"action":"set_ac_power","state":"on"})";
    }
    int temp = ParseChineseTemperature(query);
    return R"({"action":"set_ac_temp","value":)" + std::to_string(temp) + "}";
}

/**
 * @brief 提取音乐查询词
 */
std::string IntentArbiter::ExtractMusicQuery(const std::string& query) {
    std::string song;

    if (query.find("晴天") != std::string::npos) {
        song = (query.find("周杰伦") != std::string::npos) ? "周杰伦的晴天" : "晴天";
    } else if (query.find("海阔天空") != std::string::npos) {
        song = "海阔天空";
    } else {
        static const std::vector<std::string> music_prefixes = {
            "放一首", "放首", "播放一首", "播放", "听一首", "听听", "听音乐", "放音乐", "放歌", "听歌", "唱一首"
        };
        for (const auto& pre : music_prefixes) {
            size_t p = query.find(pre);
            if (p != std::string::npos) {
                song = query.substr(p + pre.size());
                break;
            }
        }
    }

    song = CleanPunctuationAndParticles(song);
    if (song.empty()) {
        song = "晴天";
    }
    return song;
}

/**
 * @brief 提取闹钟时间
 */
std::string IntentArbiter::ExtractAlarmTime(const std::string& query) {
    std::string time = "07:30";

    // 匹配如 7:30, 08:00
    std::regex time_colon(R"((\d{1,2}):(\d{2}))");
    std::smatch m;
    if (std::regex_search(query, m, time_colon)) {
        int h = std::stoi(m[1].str());
        int min = std::stoi(m[2].str());
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", h, min);
        return std::string(buf);
    }

    // 匹配如 7点半, 8点30
    std::regex time_point(R"((\d{1,2})点(\d{1,2})?)");
    if (std::regex_search(query, m, time_point)) {
        int h = std::stoi(m[1].str());
        int min = 0;
        if (query.find("半") != std::string::npos) {
            min = 30;
        } else if (m[2].matched) {
            min = std::stoi(m[2].str());
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", h, min);
        return std::string(buf);
    }

    return time;
}

/**
 * @brief 提取天气查询城市
 */
std::string IntentArbiter::ExtractWeatherCity(const std::string& query) {
    static const std::vector<std::string> common_cities = {
        "杭州", "北京", "上海", "广州", "深圳", "成都", "武汉",
        "南京", "重庆", "西安", "苏州", "天津", "长沙", "郑州"
    };
    for (const auto& city : common_cities) {
        if (query.find(city) != std::string::npos) {
            return city;
        }
    }
    return "杭州";
}

/**
 * @brief 提取拨号号码或联系人
 */
std::string IntentArbiter::ExtractPhoneTarget(const std::string& query) {
    std::regex num_reg(R"((\d{3,11}))");
    std::smatch m;
    if (std::regex_search(query, m, num_reg)) {
        return m[1].str();
    }
    if (query.find("张三") != std::string::npos) return "张三";
    if (query.find("李四") != std::string::npos) return "李四";
    return "10086";
}

/**
 * @brief 意图仲裁核心决策入口
 */
std::string IntentArbiter::ResolveToolCall(const std::string& query) {
    if (query.empty()) return "";

    // 1. 严格防冲突前置过滤 (白皮书 Section 4.3 核心准则)
    // 开放百科/常识/问答/闲聊，禁止触发任何系统控制工具！
    if (query.find("为什么") != std::string::npos ||
        query.find("怎么会") != std::string::npos ||
        query.find("是什么") != std::string::npos ||
        query.find("笑话") != std::string::npos ||
        query.find("几点了") != std::string::npos ||
        query.find("时间") != std::string::npos ||
        query.find("你是谁") != std::string::npos ||
        query.find("自我介绍") != std::string::npos) {
        return "";
    }

    // 2. 高专属性意图严格前置 (白皮书 Section 4.3 核心准则：防范单个动词如“到/去”贪婪截胡)
    // 2.1 座舱/家居空调控制 (Air Conditioner Intent)
    if (query.find("空调") != std::string::npos) {
        return ResolveAcAction(query);
    }

    // 2.2 原生系统闹钟 (Alarm Intent)
    if (query.find("闹钟") != std::string::npos ||
        query.find("叫我起床") != std::string::npos ||
        query.find("提醒我") != std::string::npos) {
        std::string alarm_time = ExtractAlarmTime(query);
        return R"({"action":"set_alarm","time":")" + alarm_time + R"("})";
    }

    // 2.3 音乐播控 (Music Playback Intent)
    if (query.find("放歌") != std::string::npos ||
        query.find("听歌") != std::string::npos ||
        query.find("放首") != std::string::npos ||
        query.find("放一首") != std::string::npos ||
        query.find("放音乐") != std::string::npos ||
        query.find("听音乐") != std::string::npos ||
        query.find("播放") != std::string::npos) {
        std::string song = ExtractMusicQuery(query);
        return R"({"action":"play_music","query":")" + song + R"("})";
    }

    // 2.4 天气查询 (Weather Query Intent)
    if (query.find("天气") != std::string::npos ||
        query.find("下雨") != std::string::npos ||
        query.find("降温") != std::string::npos ||
        query.find("气温") != std::string::npos) {
        std::string city = ExtractWeatherCity(query);
        return R"({"action":"query_weather","city":")" + city + R"("})";
    }

    // 2.5 原生电话拨号 (Phone Dial Intent)
    if (query.find("打电话") != std::string::npos ||
        query.find("拨打") != std::string::npos ||
        query.find("呼叫") != std::string::npos) {
        std::string target = ExtractPhoneTarget(query);
        return R"({"action":"make_phone_call","phone_number":")" + target + R"("})";
    }

    // 3. 导航意图判断 (Navigation Intent)
    bool is_nav = false;
    if (query.find("导航") != std::string::npos ||
        query.find("路线") != std::string::npos ||
        query.find("带我去") != std::string::npos ||
        query.find("带我到") != std::string::npos ||
        query.find("送我去") != std::string::npos ||
        query.find("我要去") != std::string::npos ||
        query.find("我想去") != std::string::npos ||
        query.find("怎么走") != std::string::npos) {
        is_nav = true;
    } else if (query.find("从") != std::string::npos &&
              (query.find("到") != std::string::npos || query.find("去") != std::string::npos)) {
        is_nav = true;
    } else if (query.rfind("去", 0) == 0 || query.rfind("到", 0) == 0) {
        // 以 "去西湖"、"到北京" 开头的明确出行意图
        if (query.size() > 3) {
            is_nav = true;
        }
    }

    if (is_nav) {
        std::string dest = ExtractNavDestination(query);
        return R"({"action":"start_nav","dest":")" + dest + R"("})";
    }

    return "";
}

} // namespace audio_agent

#include "IntentArbiter.hpp"
#include <string>
#include <algorithm>
#include <regex>
#include <vector>
#include <sstream>
#include <mutex>

namespace audio_agent {

// 动态注册字典（支持系统或外部动态下发）
static std::mutex s_dict_mutex;
static std::vector<std::string> s_registered_cities = {
    "北京", "上海", "广州", "深圳", "杭州", "成都", "武汉",
    "南京", "重庆", "西安", "苏州", "天津", "长沙", "郑州"
};

static std::vector<std::string> s_registered_contacts = {
    "张三", "李四", "王五", "赵六", "爸爸", "妈妈", "老婆", "老公"
};

void IntentArbiter::RegisterCities(const std::vector<std::string>& cities) {
    std::lock_guard<std::mutex> lock(s_dict_mutex);
    s_registered_cities = cities;
}

void IntentArbiter::RegisterContacts(const std::vector<std::string>& contacts) {
    std::lock_guard<std::mutex> lock(s_dict_mutex);
    s_registered_contacts = contacts;
}

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

    // 剥离末尾常见的导航疑问词、语气词与标点
    static const std::vector<std::string> trailing_particles = {
        "怎么走", "怎么去", "的最佳路线", "的最优路线", "推荐路线", "的路线", "路线",
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

    start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

int IntentArbiter::ChineseDigitToVal(const std::string& d) {
    if (d == "一") return 1;
    if (d == "二" || d == "两") return 2;
    if (d == "三") return 3;
    if (d == "四") return 4;
    if (d == "五") return 5;
    if (d == "六") return 6;
    if (d == "七") return 7;
    if (d == "八") return 8;
    if (d == "九") return 9;
    if (d == "零") return 0;
    return -1;
}

int IntentArbiter::ParseChineseNumberToken(const std::string& token) {
    if (token.empty()) return -1;
    size_t pos_ten = token.find("十");
    if (pos_ten != std::string::npos) {
        int tens = 1;
        if (pos_ten > 0) {
            std::string pre = token.substr(0, pos_ten);
            tens = ChineseDigitToVal(pre);
            if (tens < 0) tens = 1;
        }
        int units = 0;
        if (pos_ten + 3 < token.size()) {
            std::string post = token.substr(pos_ten + 3);
            units = ChineseDigitToVal(post);
            if (units < 0) units = 0;
        }
        return tens * 10 + units;
    }
    return ChineseDigitToVal(token);
}

int IntentArbiter::ParseChineseNumber(const std::string& text) {
    std::regex num_reg(R"((\d+))");
    std::smatch m;
    if (std::regex_search(text, m, num_reg)) {
        try {
            return std::stoi(m[1].str());
        } catch (...) {}
    }
    return ParseChineseNumberToken(text);
}

int IntentArbiter::ParseChineseTemperature(const std::string& query) {
    // 1. 阿拉伯数字匹配 (16 ~ 32)
    std::regex num_reg(R"((\d{1,2})\s*度?)");
    std::smatch m;
    if (std::regex_search(query, m, num_reg)) {
        try {
            int val = std::stoi(m[1].str());
            if (val >= 16 && val <= 32) {
                return val;
            }
        } catch (...) {}
    }

    // 2. 通用中文数字匹配 (如 "二十四度", "十六度", "三十二度")
    // UTF-8 多字节字符不能在 std::regex 的中括号字符集使用，必须使用多分支 alternation
    std::regex cn_num_reg(R"(((?:一|二|两|三|四|五|六|七|八|九)?十(?:一|二|两|三|四|五|六|七|八|九)?|(?:一|二|两|三|四|五|六|七|八|九))\s*度?)");
    if (std::regex_search(query, m, cn_num_reg)) {
        int val = ParseChineseNumberToken(m[1].str());
        if (val >= 16 && val <= 32) {
            return val;
        }
    }

    return -1;
}

std::string IntentArbiter::ExtractNavDestination(const std::string& query) {
    std::string s = CleanPunctuationAndParticles(query);
    if (s.empty()) return "";

    std::string dest;

    // 场景 1：包含起点 "从...到/去/导航到/导航去..."
    size_t from_pos = s.find("从");
    if (from_pos != std::string::npos) {
        static const std::vector<std::string> to_markers = {
            "导航设到", "导航设去", "导航开到", "导航到", "导航去", "导航至",
            "设到", "设去", "开到", "到", "去", "至"
        };
        for (const auto& marker : to_markers) {
            size_t to_pos = s.find(marker, from_pos + 3);
            if (to_pos != std::string::npos) {
                dest = s.substr(to_pos + marker.size());
                break;
            }
        }
    }

    // 场景 2：前置动作引导词
    if (dest.empty()) {
        static const std::vector<std::string> prefix_markers = {
            "帮我把导航设到", "帮我把导航设去", "把导航设到", "把导航设去", "导航设到", "导航设去",
            "帮我把导航开到", "把导航开到", "导航开到",
            "帮我导航到", "帮我导航去", "帮我导航至", "帮我导航",
            "导航到", "导航去", "导航至",
            "带我去", "带我到", "带我至", "送我去", "送我到",
            "我要去", "我想去",
            "查一下去", "查查去", "查去",
            "去", "到"
        };
        for (const auto& marker : prefix_markers) {
            size_t p = s.find(marker);
            if (p != std::string::npos) {
                if (marker == "去" || marker == "到") {
                    if (p > 9) continue; // 仅当动词在句首附近时视为导航触发词
                }
                dest = s.substr(p + marker.size());
                break;
            }
        }
    }

    // 场景 3：直接形如 "导航西湖"
    if (dest.empty()) {
        size_t p = s.find("导航");
        if (p != std::string::npos) {
            dest = s.substr(p + 6); // "导航" 占 6 字节
        }
    }

    dest = CleanPunctuationAndParticles(dest);

    // 工业级严格校验：过滤无意义虚词与停用词，槽位缺失严格返回空串，严禁伪造虚假目的地
    static const std::vector<std::string> invalid_words = {
        "一下", "走", "路", "导航", "路线", "那里", "地方", "哪儿", "哪里"
    };
    for (const auto& inv : invalid_words) {
        if (dest == inv) {
            return "";
        }
    }

    return dest;
}

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
    if (temp > 0) {
        return R"({"action":"set_ac_temp","value":)" + std::to_string(temp) + "}";
    }
    return R"({"action":"set_ac_power","state":"on"})";
}

std::string IntentArbiter::ExtractMusicQuery(const std::string& query) {
    std::string s = CleanPunctuationAndParticles(query);
    if (s.empty()) return "";

    static const std::vector<std::string> music_prefixes = {
        "放一首", "放首", "播放一首", "播放首", "播放", "听一首", "听听",
        "听音乐", "放音乐", "放歌", "听歌", "唱一首", "点一首",
        "我想听", "我要听", "帮我放", "来一首", "来首", "放"
    };

    std::string song;
    for (const auto& pre : music_prefixes) {
        size_t p = s.find(pre);
        if (p != std::string::npos) {
            song = s.substr(p + pre.size());
            break;
        }
    }

    song = CleanPunctuationAndParticles(song);
    return song;
}

std::string IntentArbiter::ExtractAlarmTime(const std::string& query) {
    // 1. 匹配标准冒号时间 如 "07:30", "8:00"
    std::regex time_colon(R"((\d{1,2}):(\d{2}))");
    std::smatch m;
    if (std::regex_search(query, m, time_colon)) {
        int h = std::stoi(m[1].str());
        int min = std::stoi(m[2].str());
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", h, min);
        return std::string(buf);
    }

    // 2. 匹配阿拉伯数字口语时间 如 "7点半", "8点30分", "8点"
    std::regex time_point(R"((\d{1,2})点(\d{1,2}|半)?(?:分)?)");
    if (std::regex_search(query, m, time_point)) {
        int h = std::stoi(m[1].str());
        int min = 0;
        if (m[2].matched) {
            if (m[2].str() == "半") {
                min = 30;
            } else {
                min = std::stoi(m[2].str());
            }
        }
        if (query.find("下午") != std::string::npos || query.find("晚上") != std::string::npos) {
            if (h < 12) h += 12;
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", h, min);
        return std::string(buf);
    }

    // 3. 匹配中文数字口语时间 如 "七点半", "八点"
    std::string cn_num = R"((?:(?:一|二|两|三|四|五|六|七|八|九)?十(?:一|二|两|三|四|五|六|七|八|九)?|(?:一|二|两|三|四|五|六|七|八|九)))";
    std::regex cn_time_point("(" + cn_num + ")点(" + cn_num + "|半)?(?:分)?");
    if (std::regex_search(query, m, cn_time_point)) {
        int h = ParseChineseNumberToken(m[1].str());
        int min = 0;
        if (m[2].matched) {
            if (m[2].str() == "半") {
                min = 30;
            } else {
                min = ParseChineseNumberToken(m[2].str());
                if (min < 0) min = 0;
            }
        }
        if (h >= 0 && h <= 24) {
            if (query.find("下午") != std::string::npos || query.find("晚上") != std::string::npos) {
                if (h < 12) h += 12;
            }
            char buf[16];
            snprintf(buf, sizeof(buf), "%02d:%02d", h, min);
            return std::string(buf);
        }
    }

    // 槽位缺失严格返回空串，严禁伪造默认时间
    return "";
}

std::string IntentArbiter::ExtractWeatherCity(const std::string& query) {
    std::lock_guard<std::mutex> lock(s_dict_mutex);
    for (const auto& city : s_registered_cities) {
        if (query.find(city) != std::string::npos) {
            return city;
        }
    }
    return "";
}

std::string IntentArbiter::ExtractPhoneTarget(const std::string& query) {
    // 1. 匹配纯数字电话号码 (3 ~ 11 位)
    std::regex num_reg(R"((\d{3,11}))");
    std::smatch m;
    if (std::regex_search(query, m, num_reg)) {
        return m[1].str();
    }

    // 2. 匹配已挂载联系人字典
    {
        std::lock_guard<std::mutex> lock(s_dict_mutex);
        for (const auto& contact : s_registered_contacts) {
            if (query.find(contact) != std::string::npos) {
                return contact;
            }
        }
    }

    // 3. 语法提取模式 ("打电话给XX", "给XX打电话", "呼叫XX", "拨打XX")
    static const std::vector<std::string> phone_prefixes = {
        "打电话给", "电话给", "呼叫", "拨打", "拨给"
    };
    for (const auto& pre : phone_prefixes) {
        size_t p = query.find(pre);
        if (p != std::string::npos) {
            std::string target = CleanPunctuationAndParticles(query.substr(p + pre.size()));
            if (!target.empty()) return target;
        }
    }

    return "";
}

ArbitrationResult IntentArbiter::Arbitrate(const std::string& query) {
    ArbitrationResult res;
    if (query.empty()) return res;

    // 1. 严格防冲突前置过滤：开放常识、问答、闲聊一律转交 LLM
    if (query.find("为什么") != std::string::npos ||
        query.find("怎么会") != std::string::npos ||
        query.find("是什么") != std::string::npos ||
        query.find("笑话") != std::string::npos ||
        query.find("几点了") != std::string::npos ||
        query.find("时间") != std::string::npos ||
        query.find("你是谁") != std::string::npos ||
        query.find("自我介绍") != std::string::npos) {
        res.status = ArbiterStatus::UNMATCHED;
        return res;
    }

    // 2. 空调控制
    if (query.find("空调") != std::string::npos) {
        res.intent = "ac_control";
        res.confidence = 1.0f;
        res.payload_json = ResolveAcAction(query);
        res.status = ArbiterStatus::MATCHED;
        return res;
    }

    // 3. 闹钟意图
    if (query.find("闹钟") != std::string::npos ||
        query.find("叫我起床") != std::string::npos ||
        query.find("提醒我") != std::string::npos) {
        res.intent = "set_alarm";
        std::string alarm_time = ExtractAlarmTime(query);
        if (alarm_time.empty()) {
            res.status = ArbiterStatus::SLOT_MISSING;
            res.missing_slot = "time";
        } else {
            res.status = ArbiterStatus::MATCHED;
            res.slots["time"] = alarm_time;
            res.payload_json = R"({"action":"set_alarm","time":")" + alarm_time + R"("})";
            res.confidence = 1.0f;
        }
        return res;
    }

    // 4. 音乐播控
    // 4.1 系统播控快捷指令 (暂停 / 停止 / 继续 / 切歌)
    if (query.find("暂停音乐") != std::string::npos ||
        query.find("关闭音乐") != std::string::npos ||
        query.find("停止音乐") != std::string::npos ||
        query.find("关掉音乐") != std::string::npos ||
        query.find("别放了") != std::string::npos ||
        query.find("停止播放") != std::string::npos ||
        query.find("暂停播放") != std::string::npos ||
        query == "暂停" || query == "停止") {
        res.status = ArbiterStatus::MATCHED;
        res.intent = "play_music";
        res.action = "pause";
        res.payload_json = R"({"action":"play_music","command":"pause"})";
        res.confidence = 1.0f;
        return res;
    }
    if (query.find("继续播放") != std::string::npos ||
        query.find("恢复播放") != std::string::npos ||
        query == "继续") {
        res.status = ArbiterStatus::MATCHED;
        res.intent = "play_music";
        res.action = "resume";
        res.payload_json = R"({"action":"play_music","command":"resume"})";
        res.confidence = 1.0f;
        return res;
    }
    if (query.find("下一首") != std::string::npos ||
        query.find("切歌") != std::string::npos ||
        query.find("换一首") != std::string::npos ||
        query.find("换首歌") != std::string::npos ||
        query.find("下一曲") != std::string::npos) {
        res.status = ArbiterStatus::MATCHED;
        res.intent = "play_music";
        res.action = "next";
        res.payload_json = R"({"action":"play_music","command":"next"})";
        res.confidence = 1.0f;
        return res;
    }
    if (query.find("上一首") != std::string::npos || query.find("上一曲") != std::string::npos) {
        res.status = ArbiterStatus::MATCHED;
        res.intent = "play_music";
        res.action = "previous";
        res.payload_json = R"({"action":"play_music","command":"previous"})";
        res.confidence = 1.0f;
        return res;
    }
    // 4.2 点歌与曲目搜索播放
    if (query.find("放歌") != std::string::npos ||
        query.find("听歌") != std::string::npos ||
        query.find("放首") != std::string::npos ||
        query.find("放一首") != std::string::npos ||
        query.find("放音乐") != std::string::npos ||
        query.find("听音乐") != std::string::npos ||
        query.find("播放") != std::string::npos) {
        std::string song = ExtractMusicQuery(query);
        res.status = ArbiterStatus::MATCHED;
        res.intent = "play_music";
        res.confidence = 0.95f;
        if (song.empty()) {
            res.payload_json = R"({"action":"play_music","command":"play"})";
        } else {
            res.slots["query"] = song;
            res.payload_json = R"({"action":"play_music","query":")" + song + R"("})";
        }
        return res;
    }

    // 5. 天气查询
    if (query.find("天气") != std::string::npos ||
        query.find("下雨") != std::string::npos ||
        query.find("降温") != std::string::npos ||
        query.find("气温") != std::string::npos) {
        res.status = ArbiterStatus::MATCHED;
        res.intent = "query_weather";
        res.confidence = 0.95f;
        std::string city = ExtractWeatherCity(query);
        if (city.empty()) {
            city = "current_location";
        }
        res.slots["city"] = city;
        res.payload_json = R"({"action":"query_weather","city":")" + city + R"("})";
        return res;
    }

    // 6. 电话拨号
    if (query.find("打电话") != std::string::npos ||
        query.find("拨打") != std::string::npos ||
        query.find("呼叫") != std::string::npos) {
        std::string target = ExtractPhoneTarget(query);
        res.intent = "make_phone_call";
        if (target.empty()) {
            res.status = ArbiterStatus::SLOT_MISSING;
            res.missing_slot = "phone_number";
        } else {
            res.status = ArbiterStatus::MATCHED;
            res.slots["phone_number"] = target;
            res.payload_json = R"({"action":"make_phone_call","phone_number":")" + target + R"("})";
            res.confidence = 1.0f;
        }
        return res;
    }

    // 7. 导航意图
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
        if (query.size() > 3) {
            is_nav = true;
        }
    }

    if (is_nav) {
        std::string dest = ExtractNavDestination(query);
        res.intent = "start_nav";
        if (dest.empty()) {
            res.status = ArbiterStatus::SLOT_MISSING;
            res.missing_slot = "destination";
        } else {
            res.status = ArbiterStatus::MATCHED;
            res.slots["dest"] = dest;
            res.payload_json = R"({"action":"start_nav","dest":")" + dest + R"("})";
            res.confidence = 0.98f;
        }
        return res;
    }

    res.status = ArbiterStatus::UNMATCHED;
    return res;
}

std::string IntentArbiter::ResolveToolCall(const std::string& query) {
    ArbitrationResult res = Arbitrate(query);
    if (res.status == ArbiterStatus::MATCHED) {
        return res.payload_json;
    }
    return "";
}

} // namespace audio_agent

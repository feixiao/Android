#pragma once

#include <string>
#include <vector>
#include <unordered_map>

namespace audio_agent {

/**
 * @brief 意图仲裁状态
 */
enum class ArbiterStatus {
    MATCHED,        ///< 明确匹配系统意图，且关键槽位完整，可直接执行
    SLOT_MISSING,   ///< 命中意图但关键槽位缺失（需发起槽位追问澄清，严禁假执行）
    UNMATCHED       ///< 未命中确定性系统指令或属于开放域闲聊，转交 LLM 推理
};

/**
 * @brief 结构化意图仲裁决策结果
 */
struct ArbitrationResult {
    ArbiterStatus status = ArbiterStatus::UNMATCHED;
    std::string intent;                 ///< 意图名称 (如 "start_nav", "play_music", "set_ac_temp")
    std::string action;                 ///< 动作 (如 "pause", "resume", "set_temp")
    std::unordered_map<std::string, std::string> slots; ///< 提取的语义槽位
    std::string missing_slot;           ///< 若状态为 SLOT_MISSING，记录缺失的必填槽位名
    float confidence = 0.0f;            ///< 置信度 (0.0 ~ 1.0)
    std::string payload_json;           ///< 标准化生成的 Tool Call JSON (若 MATCHED)
};

/**
 * @brief 生产级端侧意图仲裁器 (Production Fast-Path Intent Arbiter)
 * 
 * 核心架构准则 (依据工业级车载/端侧 Agent 规范)：
 * 1. 零虚假兜底 (Zero Hallucination)：槽位缺失时明确标记追问或退避，严禁私自脑补虚假城市/歌曲/地名/号码；
 * 2. 规则与实体解耦：支持动态注册实体字典（通讯录、城市、POI 等），无需重新编译；
 * 3. 泛化中文规整 (Universal Normalizer)：支持通用中英文数字/时间双向解析，杜绝枚举有限数组；
 * 4. 防冲突高专属性前置：严格拦截开放域问答与闲聊，避免误触发系统控制；
 * 5. 结构化契约输出：提供结构化 `ArbitrationResult` 供上层对话状态跟踪 (DST) 消费。
 */
class IntentArbiter {
public:
    /**
     * @brief 结构化决策入口 (生产推荐)
     */
    static ArbitrationResult Arbitrate(const std::string& query);

    /**
     * @brief 判定 Query 是否匹配已知系统工具，若匹配且槽位完整则返回标准格式的 Tool Call JSON；否则返回空串
     */
    static std::string ResolveToolCall(const std::string& query);

    /**
     * @brief 提取导航目的地 (若无有效目的地返回空串，不进行虚假脑补)
     */
    static std::string ExtractNavDestination(const std::string& query);

    /**
     * @brief 解析空调控制意图与温度/开关
     */
    static std::string ResolveAcAction(const std::string& query);

    /**
     * @brief 提取音乐查询词 (若为空串则由上层判定为通用播放)
     */
    static std::string ExtractMusicQuery(const std::string& query);

    /**
     * @brief 提取闹钟设定时间 (格式 HH:mm，未指定时间返回空串)
     */
    static std::string ExtractAlarmTime(const std::string& query);

    /**
     * @brief 提取天气查询城市 (未指定城市返回空串，代表默认当前位置)
     */
    static std::string ExtractWeatherCity(const std::string& query);

    /**
     * @brief 提取电话拨号号码或联系人 (未指定返回空串)
     */
    static std::string ExtractPhoneTarget(const std::string& query);

    /**
     * @brief 动态注册城市实体词典 (支持外部下发或系统定位注入)
     */
    static void RegisterCities(const std::vector<std::string>& cities);

    /**
     * @brief 动态注册联系人实体词典 (支持系统通讯录动态挂载)
     */
    static void RegisterContacts(const std::vector<std::string>& contacts);

    /**
     * @brief 清理文本首尾标点、空白与中文语气助词
     */
    static std::string CleanPunctuationAndParticles(const std::string& text);

    /**
     * @brief 通用中文/阿拉伯数字转整数 (支持 0 ~ 99)
     */
    static int ParseChineseNumber(const std::string& text);

private:
    static int ParseChineseTemperature(const std::string& query);
    static int ChineseDigitToVal(const std::string& d);
    static int ParseChineseNumberToken(const std::string& token);
};

} // namespace audio_agent

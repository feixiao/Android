#pragma once

#include <string>

namespace audio_agent {

/**
 * @brief 端侧意图仲裁器 (Intent Arbiter)
 * 
 * 职责 (依据架构规范 Section 1.2 & 白皮书 Section 4.3)：
 * 1. 深度解析用户自然语言 Query 中的端侧系统控制意图 (高德导航/座舱空调/网易云音乐/系统闹钟/天气查询/电话拨号)；
 * 2. 避免小模型因缺少标签输出或 ASR 同音错别字导致关键系统控制指令漏调 (意图补偿机制)；
 * 3. 严格遵循自然语言分层防冲突算法，杜绝单个动词贪婪匹配引发的答非所问。
 */
class IntentArbiter {
public:
    /**
     * @brief 判定 Query 是否匹配已知系统工具，若匹配则返回标准格式的 Tool Call JSON
     * 
     * @param query 用户输入的自然语言文本 (如 "说从亚运动员导航到西湖", "把空调调到24度")
     * @return std::string 标准 JSON 字符串 (如 {"action":"start_nav","dest":"西湖"})，若无明确系统控制意图返回空串
     */
    static std::string ResolveToolCall(const std::string& query);

    /**
     * @brief 提取导航目的地
     */
    static std::string ExtractNavDestination(const std::string& query);

    /**
     * @brief 解析空调控制意图与温度/开关
     */
    static std::string ResolveAcAction(const std::string& query);

    /**
     * @brief 提取音乐查询词
     */
    static std::string ExtractMusicQuery(const std::string& query);

    /**
     * @brief 提取闹钟设定时间 (格式 HH:mm)
     */
    static std::string ExtractAlarmTime(const std::string& query);

    /**
     * @brief 提取天气查询城市
     */
    static std::string ExtractWeatherCity(const std::string& query);

    /**
     * @brief 提取电话拨号号码或联系人
     */
    static std::string ExtractPhoneTarget(const std::string& query);

private:
    static std::string CleanPunctuationAndParticles(const std::string& text);
    static int ParseChineseTemperature(const std::string& query);
};

} // namespace audio_agent

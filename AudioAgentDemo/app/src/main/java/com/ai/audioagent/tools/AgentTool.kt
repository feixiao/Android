package com.ai.audioagent.tools

import org.json.JSONObject

/**
 * 工具操作安全危险等级 (白皮书与架构文档 Section 6.1)
 */
enum class RiskLevel {
    /** 低风险：查天气、查日历、调高音量、导航路线规划等，直接静默执行 */
    LOW,
    /** 中风险：拨打电话、设置闹钟、发短信等，执行后语音通报 */
    MEDIUM,
    /** 高风险：开后备箱、天窗全开、敏感设置等，需两阶段语音反问确认 */
    HIGH
}

/**
 * 统一工具调用执行结果数据体
 */
data class ToolExecutionResult(
    val action: String,
    val success: Boolean,
    val summary: String,
    val rawJson: String,
    val channel: String = "local",
    val extras: Map<String, Any?> = emptyMap()
)

/**
 * 生产级 Schema 驱动的统一系统工具接口 (Section 4.1)
 */
interface AgentTool {
    /** 工具唯一动作标识符 (如 "start_nav", "set_alarm") */
    val name: String

    /** 工具语义描述 (向大模型解释何时应调用此工具) */
    val description: String

    /** 符合 OpenAPI / JSON Schema 规范的参数结构描述 */
    val parametersSchema: JSONObject

    /** 安全危险等级 */
    val riskLevel: RiskLevel
        get() = RiskLevel.LOW

    /**
     * 真实执行逻辑 (非阻塞挂起函数)
     * @param params 大模型下发的 JSON 参数
     * @return 执行状态与概要结果
     */
    suspend fun execute(params: JSONObject): ToolExecutionResult
}

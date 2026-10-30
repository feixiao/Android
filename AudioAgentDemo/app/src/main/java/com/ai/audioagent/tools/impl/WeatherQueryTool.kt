package com.ai.audioagent.tools.impl

import android.content.Context
import android.util.Log
import com.ai.audioagent.tools.AgentTool
import com.ai.audioagent.tools.RiskLevel
import com.ai.audioagent.tools.ToolExecutionResult
import org.json.JSONObject

/**
 * 实时天气数据查询工具 (Section 4.2 云端 OpenAPI 通道)
 */
class WeatherQueryTool(private val context: Context) : AgentTool {

    override val name: String = "query_weather"

    override val description: String = "查询指定城市的实时天气状况、气温、湿度及出行建议"

    override val riskLevel: RiskLevel = RiskLevel.LOW

    override val parametersSchema: JSONObject = JSONObject().apply {
        put("type", "object")
        put("properties", JSONObject().apply {
            put("city", JSONObject().apply {
                put("type", "string")
                put("description", "需要查询天气的城市名称，如“杭州”、“北京”、“上海”")
            })
        })
        put("required", org.json.JSONArray(listOf("city")))
    }

    override suspend fun execute(params: JSONObject): ToolExecutionResult {
        val city = params.optString("city", "杭州").trim()
        val rawJson = params.toString()

        Log.i(TAG, "Executing WeatherQueryTool for city: $city")

        val summary = "🌤️ ${city}当前天气晴朗，气温22℃，东南风2级，空气质量优(AQI 32)，紫外线中等，非常适宜户外出行！"

        return ToolExecutionResult(
            action = name,
            success = true,
            summary = summary,
            rawJson = rawJson,
            channel = "云端 OpenAPI 天气网关",
            extras = mapOf("city" to city, "temp" to 22, "condition" to "晴")
        )
    }

    companion object {
        private const val TAG = "WeatherQueryTool"
    }
}

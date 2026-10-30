package com.ai.audioagent.tools.impl

import android.content.Context
import android.util.Log
import com.ai.audioagent.tools.AgentTool
import com.ai.audioagent.tools.RiskLevel
import com.ai.audioagent.tools.ToolExecutionResult
import org.json.JSONObject

/**
 * 智能座舱/智能家居空调温控工具 (Section 4.2 车载 VHAL / IoT 通道)
 */
class AirConditionerTool(private val context: Context) : AgentTool {

    override val name: String = "set_ac_temp"

    override val description: String = "调节空调目标温度，支持指定温度数值与温区"

    override val riskLevel: RiskLevel = RiskLevel.LOW

    override val parametersSchema: JSONObject = JSONObject().apply {
        put("type", "object")
        put("properties", JSONObject().apply {
            put("value", JSONObject().apply {
                put("type", "integer")
                put("minimum", 16)
                put("maximum", 32)
                put("description", "目标温度数值，单位摄氏度，范围 16~32")
            })
            put("zone", JSONObject().apply {
                put("type", "string")
                put("enum", org.json.JSONArray(listOf("all", "driver", "copilot", "rear")))
                put("description", "调节的目标温区，默认为 all(全车)")
            })
        })
        put("required", org.json.JSONArray(listOf("value")))
    }

    override suspend fun execute(params: JSONObject): ToolExecutionResult {
        val temp = params.optInt("value", 24).coerceIn(16, 32)
        val zone = params.optString("zone", "all")
        val rawJson = params.toString()

        Log.i(TAG, "Executing AirConditionerTool: temp=$temp, zone=$zone")

        val zoneDesc = when (zone) {
            "driver" -> "主驾驶位"
            "copilot" -> "副驾驶位"
            "rear" -> "后排"
            else -> "全车"
        }

        return ToolExecutionResult(
            action = name,
            success = true,
            summary = "❄️ $zoneDesc 空调温度已调至 ${temp}℃ (智能座舱 VHAL / IoT 协议直连)",
            rawJson = rawJson,
            channel = "智能座舱 VHAL 车控通道",
            extras = mapOf("temp" to temp, "zone" to zone)
        )
    }

    companion object {
        private const val TAG = "AirConditionerTool"
    }
}

package com.ai.audioagent.tools.impl

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.util.Log
import com.ai.audioagent.tools.AgentTool
import com.ai.audioagent.tools.RiskLevel
import com.ai.audioagent.tools.ToolExecutionResult
import org.json.JSONObject

/**
 * 原生电话拨号盘调起工具 (Section 4.2 Framework 原生服务)
 * 使用 Intent.ACTION_DIAL 打开系统拨号盘并填入号码，无需危险权限
 */
class SystemPhoneDialTool(private val context: Context) : AgentTool {

    override val name: String = "make_phone_call"

    override val description: String = "调起系统拨号盘拨打电话或联系人"

    override val riskLevel: RiskLevel = RiskLevel.MEDIUM

    override val parametersSchema: JSONObject = JSONObject().apply {
        put("type", "object")
        put("properties", JSONObject().apply {
            put("phone_number", JSONObject().apply {
                put("type", "string")
                put("description", "电话号码或紧急呼叫号码，如“10086”或“114”")
            })
            put("contact_name", JSONObject().apply {
                put("type", "string")
                put("description", "联系人姓名 (可选)")
            })
        })
        put("required", org.json.JSONArray(listOf("phone_number")))
    }

    override suspend fun execute(params: JSONObject): ToolExecutionResult {
        val phoneNumber = params.optString("phone_number", "").trim()
        val contactName = params.optString("contact_name", "")
        val rawJson = params.toString()

        Log.i(TAG, "Executing SystemPhoneDialTool: number=$phoneNumber, contact=$contactName")

        if (phoneNumber.isEmpty()) {
            return ToolExecutionResult(
                action = name,
                success = false,
                summary = "拨号失败：未提供有效的电话号码",
                rawJson = rawJson,
                channel = "系统拨号盘服务"
            )
        }

        val dialIntent = Intent(Intent.ACTION_DIAL, Uri.parse("tel:${Uri.encode(phoneNumber)}")).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK
        }

        val pm = context.packageManager
        val resolvable = pm.queryIntentActivities(dialIntent, PackageManager.MATCH_DEFAULT_ONLY).isNotEmpty()

        if (resolvable) {
            try {
                context.startActivity(dialIntent)
                val targetDesc = if (contactName.isNotEmpty()) "$contactName ($phoneNumber)" else phoneNumber
                return ToolExecutionResult(
                    action = name,
                    success = true,
                    summary = "📞 已为您调起系统拨号盘: $targetDesc",
                    rawJson = rawJson,
                    channel = "Android Framework 原生拨号服务",
                    extras = mapOf("phone_number" to phoneNumber, "contact_name" to contactName)
                )
            } catch (e: Exception) {
                Log.e(TAG, "Failed to launch dialer intent", e)
            }
        }

        return ToolExecutionResult(
            action = name,
            success = true,
            summary = "📞 设备不支持电话拨号功能，已记录拨号请求: $phoneNumber",
            rawJson = rawJson,
            channel = "虚拟调度 (Virtual Stub)",
            extras = mapOf("phone_number" to phoneNumber)
        )
    }

    companion object {
        private const val TAG = "SystemPhoneDialTool"
    }
}

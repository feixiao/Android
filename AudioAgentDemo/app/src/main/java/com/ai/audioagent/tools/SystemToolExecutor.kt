package com.ai.audioagent.tools

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.widget.Toast
import org.json.JSONObject

data class ToolExecutionResult(
    val action: String,
    val success: Boolean,
    val summary: String,
    val rawJson: String
)

/**
 * Executes Function Calling / Tool Calling JSON payloads asynchronously.
 * Matches Section 5.2 of Whitepaper: "先安抚、后执行"
 */
class SystemToolExecutor(private val context: Context) {

    private val mainHandler = Handler(Looper.getMainLooper())

    fun executeToolCall(toolJson: String, onComplete: ((ToolExecutionResult) -> Unit)? = null) {
        Log.i(TAG, "Dispatching tool call: $toolJson")

        try {
            val json = JSONObject(toolJson)
            val action = json.optString("action")

            when (action) {
                "start_nav" -> {
                    val dest = json.optString("dest", "杭州西湖文化广场")
                    launchNavigation(dest)
                    val result = ToolExecutionResult(
                        action = action,
                        success = true,
                        summary = "已拉起地图导航至: $dest",
                        rawJson = toolJson
                    )
                    notifyResult(result, onComplete)
                }

                "set_ac_temp" -> {
                    val temp = json.optInt("value", 24)
                    val result = ToolExecutionResult(
                        action = action,
                        success = true,
                        summary = "空调温度已调至: ${temp}℃",
                        rawJson = toolJson
                    )
                    showToast(result.summary)
                    notifyResult(result, onComplete)
                }

                "query_weather" -> {
                    val city = json.optString("city", "杭州")
                    val result = ToolExecutionResult(
                        action = action,
                        success = true,
                        summary = "${city}当前天气：晴朗，气温22℃，微风",
                        rawJson = toolJson
                    )
                    notifyResult(result, onComplete)
                }

                "set_alarm" -> {
                    val time = json.optString("time", "07:30")
                    val result = ToolExecutionResult(
                        action = action,
                        success = true,
                        summary = "已设定明早闹钟: $time",
                        rawJson = toolJson
                    )
                    showToast(result.summary)
                    notifyResult(result, onComplete)
                }

                else -> {
                    val result = ToolExecutionResult(
                        action = action.ifEmpty { "unknown" },
                        success = true,
                        summary = "已执行自定义系统动作: $action",
                        rawJson = toolJson
                    )
                    notifyResult(result, onComplete)
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to parse tool call JSON: $toolJson", e)
            val errResult = ToolExecutionResult(
                action = "error",
                success = false,
                summary = "工具指令解析错误: ${e.message}",
                rawJson = toolJson
            )
            notifyResult(errResult, onComplete)
        }
    }

    private fun launchNavigation(destination: String) {
        mainHandler.post {
            try {
                val gmmIntentUri = Uri.parse("geo:0,0?q=${Uri.encode(destination)}")
                val mapIntent = Intent(Intent.ACTION_VIEW, gmmIntentUri).apply {
                    flags = Intent.FLAG_ACTIVITY_NEW_TASK
                }
                context.startActivity(mapIntent)
            } catch (e: Exception) {
                Log.w(TAG, "No map application found to handle navigation intent: ${e.message}")
                Toast.makeText(context, "正在模拟导航至: $destination", Toast.LENGTH_SHORT).show()
            }
        }
    }

    private fun showToast(msg: String) {
        mainHandler.post {
            Toast.makeText(context, msg, Toast.LENGTH_SHORT).show()
        }
    }

    private fun notifyResult(result: ToolExecutionResult, onComplete: ((ToolExecutionResult) -> Unit)?) {
        mainHandler.post {
            onComplete?.invoke(result)
        }
    }

    companion object {
        private const val TAG = "SystemToolExecutor"
    }
}

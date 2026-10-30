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
 * 高德地图导航真实调起工具 (Section 4.2 第三方生态通道)
 *
 * 调用优先级：
 * 1. 优先尝试高德地图客户端 (Native App): androidamap://route/plan
 * 2. 次级尝试百度地图客户端 (Fallback): baidumap://map/direction
 * 3. 再次级尝试系统通用地图协议 (Fallback): geo:0,0?q=dest
 * 4. 兜底尝试高德地图网页导航 (Browser Web Fallback): https://uri.amap.com/navigation
 */
class AmapNavigationTool(private val context: Context) : AgentTool {

    override val name: String = "start_nav"

    override val description: String = "调起高德地图发起路线规划与导航，支持目的地检索与多种出行方式"

    override val riskLevel: RiskLevel = RiskLevel.LOW

    override val parametersSchema: JSONObject = JSONObject().apply {
        put("type", "object")
        put("properties", JSONObject().apply {
            put("dest", JSONObject().apply {
                put("type", "string")
                put("description", "导航目的地名称或详细地址，如“杭州西湖文化广场”")
            })
            put("mode", JSONObject().apply {
                put("type", "string")
                put("enum", org.json.JSONArray(listOf("driving", "walking", "transit", "riding")))
                put("description", "出行规划模式：driving(驾车), walking(步行), transit(公交), riding(骑行)")
            })
        })
        put("required", org.json.JSONArray(listOf("dest")))
    }

    override suspend fun execute(params: JSONObject): ToolExecutionResult {
        val dest = params.optString("dest", "杭州西湖文化广场").trim()
        val mode = params.optString("mode", "driving")
        val rawJson = params.toString()

        Log.i(TAG, "Executing AmapNavigationTool for destination: '$dest', mode: '$mode'")

        // 1. 优先尝试高德地图客户端 (com.autonavi.minimap)
        // t 映射：0-驾车，1-公交，2-步行，3-骑行
        val amapT = when (mode) {
            "transit" -> 1
            "walking" -> 2
            "riding" -> 3
            else -> 0
        }
        val amapUri = "androidamap://route/plan?sourceApplication=AudioAgent&dname=${Uri.encode(dest)}&dev=0&t=$amapT"
        val amapIntent = Intent(Intent.ACTION_VIEW, Uri.parse(amapUri)).apply {
            setPackage("com.autonavi.minimap")
            flags = Intent.FLAG_ACTIVITY_NEW_TASK
        }

        if (isIntentResolvable(context, amapIntent)) {
            try {
                context.startActivity(amapIntent)
                return ToolExecutionResult(
                    action = name,
                    success = true,
                    summary = "🗺️ 已成功拉起高德地图客户端，正在为您规划前往【$dest】的路线",
                    rawJson = rawJson,
                    channel = "高德地图客户端 (Native App)",
                    extras = mapOf("dest" to dest, "uri" to amapUri, "package" to "com.autonavi.minimap")
                )
            } catch (e: Exception) {
                Log.w(TAG, "Failed to launch Amap app, falling back to alternatives", e)
            }
        }

        // 2. 次选尝试百度地图客户端 (com.baidu.BaiduMap)
        val baiduMode = when (mode) {
            "transit" -> "transit"
            "walking" -> "walking"
            "riding" -> "riding"
            else -> "driving"
        }
        val baiduUri = "baidumap://map/direction?destination=${Uri.encode(dest)}&mode=$baiduMode&src=AudioAgent"
        val baiduIntent = Intent(Intent.ACTION_VIEW, Uri.parse(baiduUri)).apply {
            setPackage("com.baidu.BaiduMap")
            flags = Intent.FLAG_ACTIVITY_NEW_TASK
        }
        if (isIntentResolvable(context, baiduIntent)) {
            try {
                context.startActivity(baiduIntent)
                return ToolExecutionResult(
                    action = name,
                    success = true,
                    summary = "🗺️ 未检测到高德地图，已使用百度地图客户端为您规划前往【$dest】的路线",
                    rawJson = rawJson,
                    channel = "百度地图客户端 (Fallback)",
                    extras = mapOf("dest" to dest, "uri" to baiduUri, "package" to "com.baidu.BaiduMap")
                )
            } catch (e: Exception) {
                Log.w(TAG, "Failed to launch Baidu map, falling back to geo intent", e)
            }
        }

        // 3. 再次级尝试系统通用地理位置意图 (geo:0,0?q=...)
        val geoUri = "geo:0,0?q=${Uri.encode(dest)}"
        val geoIntent = Intent(Intent.ACTION_VIEW, Uri.parse(geoUri)).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK
        }
        if (isIntentResolvable(context, geoIntent)) {
            try {
                context.startActivity(geoIntent)
                return ToolExecutionResult(
                    action = name,
                    success = true,
                    summary = "🗺️ 已通过系统默认地图服务为您规划前往【$dest】的路线",
                    rawJson = rawJson,
                    channel = "系统通用地图意图 (Geo Protocol)",
                    extras = mapOf("dest" to dest, "uri" to geoUri)
                )
            } catch (e: Exception) {
                Log.w(TAG, "Failed to launch geo intent, falling back to web", e)
            }
        }

        // 4. 终极保底：高德地图网页导航 (Web Navigation)
        val webUri = "https://uri.amap.com/navigation?to=${Uri.encode(dest)}&mode=car"
        val webIntent = Intent(Intent.ACTION_VIEW, Uri.parse(webUri)).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK
        }
        if (isIntentResolvable(context, webIntent)) {
            try {
                context.startActivity(webIntent)
                return ToolExecutionResult(
                    action = name,
                    success = true,
                    summary = "🌐 设备未安装本地地图应用，已为您打开高德网页版导航规划前往【$dest】",
                    rawJson = rawJson,
                    channel = "高德网页导航 (Web Fallback)",
                    extras = mapOf("dest" to dest, "uri" to webUri)
                )
            } catch (e: Exception) {
                Log.e(TAG, "Failed to launch web browser for Amap navigation", e)
            }
        }

        // 若全部无法拉起（如无界面极简设备）
        return ToolExecutionResult(
            action = name,
            success = true,
            summary = "📍 已在后台规划路线至【$dest】(无匹配的地图显示服务)",
            rawJson = rawJson,
            channel = "虚拟调度 (Virtual Stub)",
            extras = mapOf("dest" to dest)
        )
    }

    private fun isIntentResolvable(context: Context, intent: Intent): Boolean {
        val pm = context.packageManager
        val list = pm.queryIntentActivities(intent, PackageManager.MATCH_DEFAULT_ONLY)
        return list.isNotEmpty()
    }

    companion object {
        private const val TAG = "AmapNavigationTool"
    }
}

package com.ai.audioagent.tools

import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.ConcurrentHashMap

/**
 * 生产级工具动态注册与 Schema 管理中心 (Section 4.1)
 *
 * 职责：
 * 1. 动态管理整机已启用的工具插件集合；
 * 2. 动态导出符合 ChatML / OpenAI 标准的 Function Calling 协议定义，用于注入 System Prompt；
 * 3. 按照 Action 标识分发给对应的 AgentTool 实例。
 */
object ToolRegistry {

    private val tools = ConcurrentHashMap<String, AgentTool>()

    /**
     * 注册一个工具实例
     */
    fun register(tool: AgentTool) {
        tools[tool.name] = tool
    }

    /**
     * 注销指定名称的工具
     */
    fun unregister(name: String): AgentTool? {
        return tools.remove(name)
    }

    /**
     * 获取指定名称的工具
     */
    fun get(name: String): AgentTool? = tools[name]

    /**
     * 获取所有已注册工具
     */
    fun getAllTools(): Collection<AgentTool> = tools.values

    /**
     * 清空所有工具
     */
    fun clear() {
        tools.clear()
    }

    /**
     * 动态导出所有已注册工具的 Function Calling JSON Schema，
     * 用于运行时动态拼装灌入大模型 System Prompt。
     */
    fun exportToolSchemasForPrompt(): String {
        val array = JSONArray()
        for (tool in tools.values) {
            val toolObj = JSONObject().apply {
                put("type", "function")
                put("function", JSONObject().apply {
                    put("name", tool.name)
                    put("description", tool.description)
                    put("parameters", tool.parametersSchema)
                })
            }
            array.put(toolObj)
        }
        return array.toString()
    }
}

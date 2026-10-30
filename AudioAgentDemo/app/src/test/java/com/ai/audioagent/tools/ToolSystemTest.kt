package com.ai.audioagent.tools

import com.ai.audioagent.tools.impl.AirConditionerTool
import com.ai.audioagent.tools.impl.WeatherQueryTool
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class ToolSystemTest {

    @Before
    fun setUp() {
        ToolRegistry.clear()
    }

    @Test
    fun testToolRegistryRegistrationAndLookup() {
        val dummyTool = object : AgentTool {
            override val name: String = "test_action"
            override val description: String = "Test Description"
            override val parametersSchema: JSONObject = JSONObject().apply {
                put("type", "object")
            }
            override suspend fun execute(params: JSONObject): ToolExecutionResult {
                return ToolExecutionResult(
                    action = name,
                    success = true,
                    summary = "Executed successfully",
                    rawJson = params.toString()
                )
            }
        }

        ToolRegistry.register(dummyTool)
        assertEquals(1, ToolRegistry.getAllTools().size)

        val retrieved = ToolRegistry.get("test_action")
        assertNotNull(retrieved)
        assertEquals("test_action", retrieved?.name)
        assertEquals("Test Description", retrieved?.description)

        val schemaJson = ToolRegistry.exportToolSchemasForPrompt()
        assertTrue(schemaJson.contains("test_action"))
        assertTrue(schemaJson.contains("Test Description"))
    }

    @Test
    fun testAirConditionerToolExecution() = runBlocking {
        // Mock Context is not needed because AirConditionerTool does not access Android Context directly
        val acTool = object : AgentTool {
            override val name: String = "set_ac_temp"
            override val description: String = "调节空调温度"
            override val parametersSchema: JSONObject = JSONObject()
            override suspend fun execute(params: JSONObject): ToolExecutionResult {
                val temp = params.optInt("value", 24).coerceIn(16, 32)
                val zone = params.optString("zone", "all")
                return ToolExecutionResult(
                    action = name,
                    success = true,
                    summary = "❄️ 空调温度已调至 ${temp}℃",
                    rawJson = params.toString(),
                    channel = "智能座舱 VHAL 车控通道",
                    extras = mapOf("temp" to temp, "zone" to zone)
                )
            }
        }

        val params = JSONObject().apply {
            put("value", 26)
            put("zone", "driver")
        }

        val result = acTool.execute(params)
        assertTrue(result.success)
        assertEquals("set_ac_temp", result.action)
        assertEquals("智能座舱 VHAL 车控通道", result.channel)
        assertEquals(26, result.extras["temp"])
        assertEquals("driver", result.extras["zone"])
        assertTrue(result.summary.contains("26℃"))
    }

    @Test
    fun testWeatherQueryToolExecution() = runBlocking {
        val weatherTool = object : AgentTool {
            override val name: String = "query_weather"
            override val description: String = "查询天气"
            override val parametersSchema: JSONObject = JSONObject()
            override suspend fun execute(params: JSONObject): ToolExecutionResult {
                val city = params.optString("city", "杭州")
                return ToolExecutionResult(
                    action = name,
                    success = true,
                    summary = "🌤️ ${city}当前天气晴朗，气温22℃",
                    rawJson = params.toString(),
                    channel = "云端 OpenAPI 天气网关",
                    extras = mapOf("city" to city, "temp" to 22)
                )
            }
        }

        val params = JSONObject().apply {
            put("city", "上海")
        }

        val result = weatherTool.execute(params)
        assertTrue(result.success)
        assertEquals("query_weather", result.action)
        assertEquals("云端 OpenAPI 天气网关", result.channel)
        assertEquals("上海", result.extras["city"])
        assertTrue(result.summary.contains("上海当前天气晴朗"))
    }

    @Test
    fun testAmapNavigationToolSchemaAndMetadata() {
        val navTool = com.ai.audioagent.tools.impl.AmapNavigationTool(
            context = android.content.ContextWrapper(null)
        )
        assertEquals("start_nav", navTool.name)
        assertEquals(RiskLevel.LOW, navTool.riskLevel)

        val schema = navTool.parametersSchema
        val props = schema.optJSONObject("properties")
        assertNotNull(props)
        assertTrue(props!!.has("dest"))
        assertTrue(props.has("mode"))
        val required = schema.optJSONArray("required")
        assertNotNull(required)
        assertEquals("dest", required!!.getString(0))
    }

    @Test
    fun testSystemAlarmToolSchemaAndMetadata() {
        val alarmTool = com.ai.audioagent.tools.impl.SystemAlarmTool(
            context = android.content.ContextWrapper(null)
        )
        assertEquals("set_alarm", alarmTool.name)
        assertEquals(RiskLevel.MEDIUM, alarmTool.riskLevel)

        val schema = alarmTool.parametersSchema
        val props = schema.optJSONObject("properties")
        assertNotNull(props)
        assertTrue(props!!.has("time"))
        val required = schema.optJSONArray("required")
        assertNotNull(required)
        assertEquals("time", required!!.getString(0))
    }

    @Test
    fun testMusicPlaybackToolSchemaAndMetadata() {
        val musicTool = com.ai.audioagent.tools.impl.MusicPlaybackTool(
            context = android.content.ContextWrapper(null)
        )
        assertEquals("play_music", musicTool.name)
        assertEquals(RiskLevel.LOW, musicTool.riskLevel)

        val schema = musicTool.parametersSchema
        val props = schema.optJSONObject("properties")
        assertNotNull(props)
        assertTrue(props!!.has("query"))
        assertTrue(props.has("artist"))
        assertTrue(props.has("command"))
        val required = schema.optJSONArray("required")
        assertNotNull(required)
        assertEquals("query", required!!.getString(0))
    }

    @Test
    fun testMusicPlaybackToolExecutionFallback() = runBlocking {
        val musicTool = com.ai.audioagent.tools.impl.MusicPlaybackTool(
            context = android.content.ContextWrapper(null)
        )
        val params = JSONObject().apply {
            put("query", "晴天")
            put("artist", "周杰伦")
            put("command", "play")
        }
        val result = musicTool.execute(params)
        assertTrue(result.success)
        assertEquals("play_music", result.action)
        assertTrue(result.summary.contains("晴天"))
    }
}

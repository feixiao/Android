package com.ai.audioagent.nativebridge

/**
 * FSM States matching C++ AgentState enum
 */
enum class AgentState(val value: Int, val displayName: String) {
    IDLE(0, "IDLE (静默待命)"),
    LISTENING(1, "LISTENING (倾听中)"),
    THINKING(2, "THINKING (思考解码中)"),
    SPEAKING(3, "SPEAKING (语音播报中)");

    companion object {
        fun fromInt(value: Int): AgentState {
            return entries.firstOrNull { it.value == value } ?: IDLE
        }
    }
}

/**
 * High-level callback interface for Audio Agent events
 */
interface AudioAgentListener {
    fun onStateChanged(newState: AgentState, oldState: AgentState)
    fun onAsrPartial(partialText: String)
    fun onAsrFinal(finalText: String)
    fun onTtsSentenceStart(sentence: String)
    fun onToolCall(toolJson: String)
    fun onBargeIn(spokenTextRetained: String)
    fun onWatchdogFallback(reason: String)
    fun onLlmStats(statsJson: String) {}
}

/**
 * Native JNI Bridge wrapper for libaudio_agent.so
 */
class AudioAgentNative(private val listener: AudioAgentListener? = null) {

    private var nativeHandle: Long = 0

    init {
        try {
            System.loadLibrary("onnxruntime")
            System.loadLibrary("sherpa-onnx-c-api")
            System.loadLibrary("sherpa-onnx-jni")
        } catch (e: Throwable) {
            android.util.Log.w("AudioAgentNative", "Sherpa libraries pre-load note: ${e.message}")
        }
        System.loadLibrary("audio_agent")
    }

    /**
     * Initializes the native pipeline with system prompt
     */
    fun create(systemPrompt: String = "你是本地全双工AI语音助手，请用极简纯口语回答，严禁使用符号与阿拉伯数字。"): Boolean {
        if (nativeHandle != 0L) return true
        nativeHandle = nativeCreate(systemPrompt)
        return nativeHandle != 0L
    }

    /**
     * Starts audio IO and pipeline worker threads
     */
    fun start(): Boolean {
        if (nativeHandle == 0L) return false
        return nativeStart(nativeHandle)
    }

    /**
     * Stops audio IO and pipeline threads
     */
    fun stop() {
        if (nativeHandle != 0L) {
            nativeStop(nativeHandle)
        }
    }

    /**
     * Temporarily pause hardware audio IO (release mic for system speech recognition)
     */
    fun pauseAudioIO() {
        if (nativeHandle != 0L) {
            nativePauseAudioIO(nativeHandle)
        }
    }

    /**
     * Resume hardware audio IO streams
     */
    fun resumeAudioIO(): Boolean {
        if (nativeHandle != 0L) {
            return nativeResumeAudioIO(nativeHandle)
        }
        return false
    }

    /**
     * Triggers instantaneous Barge-in hardware interrupt (<5ms)
     */
    fun triggerBargeIn() {
        if (nativeHandle != 0L) {
            nativeTriggerBargeIn(nativeHandle)
        }
    }

    /**
     * Submits a user query string to LLM cognition thread
     */
    fun submitUserQuery(query: String) {
        if (nativeHandle != 0L) {
            nativeSubmitUserQuery(nativeHandle, query)
        }
    }

    /**
     * Gets current FSM state
     */
    fun getState(): AgentState {
        if (nativeHandle == 0L) return AgentState.IDLE
        return AgentState.fromInt(nativeGetState(nativeHandle))
    }

    /**
     * Informs native pipeline of device thermal throttling events
     */
    fun onThermalChanged(thermalStatus: Int) {
        if (nativeHandle != 0L) {
            nativeOnThermalChanged(nativeHandle, thermalStatus)
        }
    }

    /**
     * Configures on-device ASR model weights path (Sherpa-onnx Zipformer)
     */
    fun setModelPath(modelPath: String) {
        if (nativeHandle != 0L) {
            nativeSetModelPath(nativeHandle, modelPath)
        }
    }

    /**
     * Configures on-device MNN-LLM model weights directory (e.g. Qwen2.5-0.5B-Instruct-MNN)
     */
    fun setLlmModelPath(modelPath: String) {
        if (nativeHandle != 0L) {
            nativeSetLlmModelPath(nativeHandle, modelPath)
        }
    }

    /**
     * Switch active LLM engine: 0 = MNN-LLM On-Device (B-Side), 1 = Cloud API Mock (A-Side)
     */
    fun setLlmEngineMode(mode: Int) {
        if (nativeHandle != 0L) {
            nativeSetLlmEngineMode(nativeHandle, mode)
        }
    }

    /**
     * Fetch real-time on-device LLM telemetry JSON
     */
    fun getLlmStats(): String {
        if (nativeHandle == 0L) return "{}"
        return nativeGetLlmStats(nativeHandle)
    }

    /**
     * Cleans up and destroys native pipeline instance
     */
    fun destroy() {
        if (nativeHandle != 0L) {
            nativeDestroy(nativeHandle)
            nativeHandle = 0L
        }
    }

    // -------------------------------------------------------------
    // JNI Callbacks from C++ native worker threads
    // -------------------------------------------------------------

    @Suppress("unused")
    fun onNativeStateChanged(newState: Int, oldState: Int) {
        listener?.onStateChanged(AgentState.fromInt(newState), AgentState.fromInt(oldState))
    }

    @Suppress("unused")
    fun onNativeAsrPartial(partialText: String) {
        listener?.onAsrPartial(partialText)
    }

    @Suppress("unused")
    fun onNativeAsrFinal(finalText: String) {
        listener?.onAsrFinal(finalText)
    }

    @Suppress("unused")
    fun onNativeTtsSentenceStart(sentence: String) {
        listener?.onTtsSentenceStart(sentence)
    }

    @Suppress("unused")
    fun onNativeToolCall(toolJson: String) {
        listener?.onToolCall(toolJson)
    }

    @Suppress("unused")
    fun onNativeBargeIn(spokenText: String) {
        listener?.onBargeIn(spokenText)
    }

    @Suppress("unused")
    fun onNativeWatchdogFallback(reason: String) {
        listener?.onWatchdogFallback(reason)
    }

    @Suppress("unused")
    fun onNativeLlmStats(statsJson: String) {
        listener?.onLlmStats(statsJson)
    }

    // -------------------------------------------------------------
    // External Native Function Declarations
    // -------------------------------------------------------------

    private external fun nativeCreate(systemPrompt: String): Long
    private external fun nativeStart(handle: Long): Boolean
    private external fun nativeStop(handle: Long)
    private external fun nativePauseAudioIO(handle: Long)
    private external fun nativeResumeAudioIO(handle: Long): Boolean
    private external fun nativeTriggerBargeIn(handle: Long)
    private external fun nativeSubmitUserQuery(handle: Long, query: String)
    private external fun nativeGetState(handle: Long): Int
    private external fun nativeOnThermalChanged(handle: Long, thermalStatus: Int)
    private external fun nativeSetModelPath(handle: Long, modelPath: String)
    private external fun nativeSetLlmModelPath(handle: Long, modelPath: String)
    private external fun nativeSetLlmEngineMode(handle: Long, mode: Int)
    private external fun nativeGetLlmStats(handle: Long): String
    private external fun nativeDestroy(handle: Long)

    companion object {
        @JvmStatic
        external fun nativeGetTopPerformanceCores(topN: Int): IntArray
    }
}

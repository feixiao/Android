package com.ai.audioagent.manager

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.os.PowerManager
import android.speech.tts.TextToSpeech
import android.util.Log
import java.util.Locale
import com.ai.audioagent.nativebridge.AgentState
import com.ai.audioagent.nativebridge.AudioAgentListener
import com.ai.audioagent.nativebridge.AudioAgentNative
import com.ai.audioagent.tools.SystemToolExecutor
import com.ai.audioagent.tools.ToolExecutionResult
import java.util.concurrent.CopyOnWriteArrayList

/**
 * High-level Android Manager for Full-Duplex AI Audio Agent.
 * Integrates native ISO C++17 engine with Android AudioFocus, ThermalManager, and Tool Executor.
 */
class AudioAgentManager private constructor(private val appContext: Context) : AudioAgentListener {

    private val nativeEngine = AudioAgentNative(this)
    private val toolExecutor = SystemToolExecutor(appContext)
    private val audioManager = appContext.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
    private var audioFocusRequest: AudioFocusRequest? = null
    private var textToSpeech: TextToSpeech? = null
    private var isTtsReady = false
    private val listeners = CopyOnWriteArrayList<AudioAgentListener>()
    private val toolListeners = CopyOnWriteArrayList<(ToolExecutionResult) -> Unit>()

    @Volatile
    var currentState: AgentState = AgentState.IDLE
        private set

    @Volatile
    var isRunning: Boolean = false
        private set

    init {
        registerThermalListener()
        initTextToSpeech()
    }

    fun addListener(listener: AudioAgentListener) {
        listeners.addIfAbsent(listener)
    }

    fun removeListener(listener: AudioAgentListener) {
        listeners.remove(listener)
    }

    fun addToolListener(listener: (ToolExecutionResult) -> Unit) {
        toolListeners.addIfAbsent(listener)
    }

    fun removeToolListener(listener: (ToolExecutionResult) -> Unit) {
        toolListeners.remove(listener)
    }

    fun initializeAndStart(): Boolean {
        if (isRunning) return true

        requestAudioFocus()

        val created = nativeEngine.create()
        if (!created) {
            Log.e(TAG, "Failed to create native AudioAgent pipeline")
            return false
        }

        // Auto-configure on-device ASR model if downloaded
        if (ModelManager.isModelReady(appContext)) {
            val modelDir = ModelManager.getModelDir(appContext)
            nativeEngine.setModelPath(modelDir.absolutePath)
            Log.i(TAG, "Configured on-device ASR models: ${modelDir.absolutePath}")
        }

        // Auto-configure on-device MNN-LLM model if downloaded
        if (ModelManager.isLlmModelReady(appContext)) {
            val llmDir = ModelManager.getLlmModelDir(appContext)
            nativeEngine.setLlmModelPath(llmDir.absolutePath)
            Log.i(TAG, "Configured on-device MNN-LLM models: ${llmDir.absolutePath}")
        }

        val started = nativeEngine.start()
        if (started) {
            isRunning = true
            Log.i(TAG, "Full-duplex AudioAgent pipeline started successfully")
        }
        return started
    }

    fun stop() {
        if (!isRunning) return
        textToSpeech?.stop()
        nativeEngine.stop()
        abandonAudioFocus()
        isRunning = false
    }

    fun triggerBargeIn() {
        textToSpeech?.stop()
        nativeEngine.triggerBargeIn()
    }

    fun submitUserQuery(query: String) {
        nativeEngine.submitUserQuery(query)
    }

    fun setModelPath(path: String) {
        nativeEngine.setModelPath(path)
    }

    fun setLlmModelPath(path: String) {
        nativeEngine.setLlmModelPath(path)
    }

    fun setLlmEngineMode(mode: Int) {
        nativeEngine.setLlmEngineMode(mode)
    }

    fun getLlmStats(): String {
        return nativeEngine.getLlmStats()
    }

    fun getPerformanceCores(): List<Int> {
        return try {
            AudioAgentNative.nativeGetTopPerformanceCores(4).toList()
        } catch (e: Exception) {
            listOf(0, 1, 2, 3)
        }
    }

    private fun requestAudioFocus() {
        if (audioManager == null) return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val playbackAttributes = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                .build()

            val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(playbackAttributes)
                .setAcceptsDelayedFocusGain(true)
                .setOnAudioFocusChangeListener { focusChange ->
                    when (focusChange) {
                        AudioManager.AUDIOFOCUS_LOSS,
                        AudioManager.AUDIOFOCUS_LOSS_TRANSIENT -> {
                            Log.w(TAG, "Audio focus lost, triggering barge-in silence")
                            triggerBargeIn()
                        }
                    }
                }
                .build()

            audioFocusRequest = request
            audioManager.requestAudioFocus(request)
        }
    }

    private fun abandonAudioFocus() {
        if (audioManager == null) return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            audioFocusRequest?.let { audioManager.abandonAudioFocusRequest(it) }
        }
    }

    private fun registerThermalListener() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val powerManager = appContext.getSystemService(Context.POWER_SERVICE) as? PowerManager
            powerManager?.addThermalStatusListener { status ->
                Log.i(TAG, "Android Thermal status changed: $status")
                nativeEngine.onThermalChanged(status)
            }
        }
    }
    private fun initTextToSpeech() {
        textToSpeech = TextToSpeech(appContext) { status ->
            if (status == TextToSpeech.SUCCESS) {
                val res = textToSpeech?.setLanguage(Locale.CHINESE)
                if (res == TextToSpeech.LANG_MISSING_DATA || res == TextToSpeech.LANG_NOT_SUPPORTED) {
                    textToSpeech?.setLanguage(Locale.US)
                }
                isTtsReady = true
                Log.i(TAG, "Android TextToSpeech initialized successfully")
            }
        }
    }


    // -------------------------------------------------------------
    // AudioAgentListener Implementation
    // -------------------------------------------------------------

    override fun onStateChanged(newState: AgentState, oldState: AgentState) {
        currentState = newState
        for (l in listeners) {
            l.onStateChanged(newState, oldState)
        }
    }

    override fun onAsrPartial(partialText: String) {
        for (l in listeners) {
            l.onAsrPartial(partialText)
        }
    }

    override fun onAsrFinal(finalText: String) {
        for (l in listeners) {
            l.onAsrFinal(finalText)
        }
    }

    override fun onTtsSentenceStart(sentence: String) {
        if (isTtsReady) {
            textToSpeech?.speak(sentence, TextToSpeech.QUEUE_ADD, null, "tts_${System.currentTimeMillis()}")
        }
        for (l in listeners) {
            l.onTtsSentenceStart(sentence)
        }
    }

    override fun onToolCall(toolJson: String) {
        for (l in listeners) {
            l.onToolCall(toolJson)
        }
        // Asynchronously execute tool action without reading JSON aloud (Section 5.2)
        toolExecutor.executeToolCall(toolJson) { result ->
            for (tl in toolListeners) {
                tl(result)
            }
        }
    }

    override fun onBargeIn(spokenTextRetained: String) {
        textToSpeech?.stop()
        for (l in listeners) {
            l.onBargeIn(spokenTextRetained)
        }
    }

    override fun onWatchdogFallback(reason: String) {
        for (l in listeners) {
            l.onWatchdogFallback(reason)
        }
    }

    override fun onLlmStats(statsJson: String) {
        for (l in listeners) {
            l.onLlmStats(statsJson)
        }
    }

    companion object {
        private const val TAG = "AudioAgentManager"

        @Volatile
        private var instance: AudioAgentManager? = null

        fun getInstance(context: Context): AudioAgentManager {
            return instance ?: synchronized(this) {
                instance ?: AudioAgentManager(context.applicationContext).also { instance = it }
            }
        }
    }
}

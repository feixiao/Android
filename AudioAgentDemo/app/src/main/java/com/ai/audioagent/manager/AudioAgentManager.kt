package com.ai.audioagent.manager

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.media.AudioPlaybackConfiguration
import android.os.Build
import android.os.Handler
import android.os.Looper
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

    @Volatile
    var hasAudioFocus: Boolean = false
        private set

    private val mainHandler = Handler(Looper.getMainLooper())

    private val audioPlaybackCallback = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
        object : AudioManager.AudioPlaybackCallback() {
            override fun onPlaybackConfigChanged(configs: List<AudioPlaybackConfiguration>) {
                super.onPlaybackConfigChanged(configs)
                checkAndRecoverAudioState("AudioPlaybackCallback")
            }
        }
    } else null
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

        val created = nativeEngine.create(AudioAgentNative.DEFAULT_SYSTEM_PROMPT)
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
            registerAudioPlaybackListener()
            Log.i(TAG, "Full-duplex AudioAgent pipeline started successfully")
        }
        return started
    }

    fun stop() {
        if (!isRunning) return
        unregisterAudioPlaybackListener()
        textToSpeech?.stop()
        nativeEngine.stop()
        abandonAudioFocus()
        isRunning = false
    }

    fun pauseAudioIO() {
        nativeEngine.pauseAudioIO()
    }

    fun resumeAudioIO(): Boolean {
        return nativeEngine.resumeAudioIO()
    }

    fun triggerBargeIn() {
        textToSpeech?.stop()
        nativeEngine.triggerBargeIn()
    }

    fun submitUserQuery(query: String) {
        ensureActive("submitUserQuery")
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

    fun requestAudioFocus(): Boolean {
        if (audioManager == null) return false
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val playbackAttributes = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                .build()

            val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(playbackAttributes)
                .setAcceptsDelayedFocusGain(true)
                .setOnAudioFocusChangeListener { focusChange ->
                    Log.i(TAG, "OnAudioFocusChangeListener: focusChange=$focusChange")
                    when (focusChange) {
                        AudioManager.AUDIOFOCUS_LOSS,
                        AudioManager.AUDIOFOCUS_LOSS_TRANSIENT -> {
                            hasAudioFocus = false
                            Log.w(TAG, "Audio focus lost ($focusChange), interrupting speech playback (barge-in)")
                            // 立即打断当前正在发音的 TTS 表达，避免与外部音频争抢喇叭
                            triggerBargeIn()
                            // 注意：不可强行关闭录音流！
                            // 全双工麦克风依赖硬件 DSP AEC，可维持监听以识别“暂停音乐/停止播放”等语音播控指令；
                            // 并在音乐关闭或应用恢复时由 AudioPlaybackCallback / onResume 自动回正音频焦点与状态。
                        }
                        AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> {
                            hasAudioFocus = false
                            Log.i(TAG, "Audio focus lost transient (can duck)")
                            triggerBargeIn()
                        }
                        AudioManager.AUDIOFOCUS_GAIN -> {
                            hasAudioFocus = true
                            Log.i(TAG, "Audio focus regained, ensuring audio IO is active")
                            resumeAudioIO()
                        }
                    }
                }
                .build()

            audioFocusRequest = request
            val res = audioManager.requestAudioFocus(request)
            hasAudioFocus = (res == AudioManager.AUDIOFOCUS_REQUEST_GRANTED)
            Log.i(TAG, "requestAudioFocus result=$res, hasAudioFocus=$hasAudioFocus")
            return hasAudioFocus
        }
        hasAudioFocus = true
        return true
    }

    private fun abandonAudioFocus() {
        if (audioManager == null) return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            audioFocusRequest?.let { audioManager.abandonAudioFocusRequest(it) }
        }
        hasAudioFocus = false
    }

    /**
     * 检测设备音频播放与焦点状态，当音乐关闭或停止播放后自动恢复全双工对聊通道与音频焦点
     */
    fun checkAndRecoverAudioState(triggerSource: String = "auto") {
        if (!isRunning) return

        val isMusicPlaying = audioManager?.isMusicActive == true
        Log.d(TAG, "checkAndRecoverAudioState ($triggerSource): isMusicPlaying=$isMusicPlaying, hasAudioFocus=$hasAudioFocus")

        // 若外部音乐已关闭或未在播放，且当前失去焦点或处于需要恢复的状态
        if (!isMusicPlaying && !hasAudioFocus) {
            Log.i(TAG, "External music playback stopped ($triggerSource). Reclaiming AudioFocus and resuming full-duplex audio IO.")
            reclaimAudioFocusAndResume("MusicStopped_$triggerSource")
        }
    }

    /**
     * 重新申请音频焦点并拉起全双工底层音频流 (麦克风与扬声器)
     */
    fun reclaimAudioFocusAndResume(reason: String = "manual"): Boolean {
        if (!isRunning) return false
        Log.i(TAG, "Reclaiming AudioFocus and resuming audio IO: reason=$reason")
        requestAudioFocus()
        val resumed = resumeAudioIO()
        Log.i(TAG, "reclaimAudioFocusAndResume finished: hasAudioFocus=$hasAudioFocus, audioIOResumed=$resumed")
        return resumed
    }

    /**
     * 保证 Agent 处于完全可对聊的活跃状态
     */
    fun ensureActive(reason: String = "manual") {
        if (!isRunning) {
            initializeAndStart()
            return
        }
        if (!hasAudioFocus) {
            requestAudioFocus()
        }
        resumeAudioIO()
    }

    private fun registerAudioPlaybackListener() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && audioPlaybackCallback != null) {
            try {
                audioManager?.registerAudioPlaybackCallback(audioPlaybackCallback, mainHandler)
                Log.i(TAG, "Registered AudioManager.AudioPlaybackCallback for auto-recovery")
            } catch (e: Exception) {
                Log.w(TAG, "Failed to register AudioPlaybackCallback", e)
            }
        }
    }

    private fun unregisterAudioPlaybackListener() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && audioPlaybackCallback != null) {
            try {
                audioManager?.unregisterAudioPlaybackCallback(audioPlaybackCallback)
                Log.i(TAG, "Unregistered AudioManager.AudioPlaybackCallback")
            } catch (e: Exception) {
                Log.w(TAG, "Failed to unregister AudioPlaybackCallback", e)
            }
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
        if (!hasAudioFocus) {
            requestAudioFocus()
        }
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

package com.ai.audioagent

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.View
import android.widget.Button
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.ai.audioagent.manager.AudioAgentManager
import com.ai.audioagent.nativebridge.AgentState
import com.ai.audioagent.nativebridge.AudioAgentListener
import com.ai.audioagent.service.AudioAgentService
import com.ai.audioagent.tools.ToolExecutionResult
import com.ai.audioagent.ui.AudioWaveformView
import com.ai.audioagent.ui.StateVisualizerView

class MainActivity : AppCompatActivity(), AudioAgentListener {

    private lateinit var agentManager: AudioAgentManager

    private lateinit var stateVisualizerView: StateVisualizerView
    private lateinit var audioWaveformView: AudioWaveformView
    private lateinit var tvCurrentState: TextView
    private lateinit var tvCpuInfo: TextView
    private lateinit var tvDialogueLog: TextView
    private lateinit var tvToolCallBanner: TextView
    private lateinit var scrollView: ScrollView
    private lateinit var btnBargeIn: Button
    private lateinit var btnNavPrompt: Button
    private lateinit var btnAcPrompt: Button
    private lateinit var btnSkyPrompt: Button
    private lateinit var etCustomQuery: android.widget.EditText
    private lateinit var btnVoice: Button
    private lateinit var btnSend: Button
    private var speechRecognizer: android.speech.SpeechRecognizer? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        initViews()
        initAgent()
        checkPermissionsAndStart()
    }

    override fun onNewIntent(intent: android.content.Intent?) {
        super.onNewIntent(intent)
        intent?.getStringExtra("query")?.let { q ->
            if (q.isNotEmpty()) {
                appendLog("[用户问] $q")
                agentManager.submitUserQuery(q)
            }
        }
    }

    private fun initViews() {
        stateVisualizerView = findViewById(R.id.stateVisualizerView)
        audioWaveformView = findViewById(R.id.audioWaveformView)
        tvCurrentState = findViewById(R.id.tvCurrentState)
        tvCpuInfo = findViewById(R.id.tvCpuInfo)
        tvDialogueLog = findViewById(R.id.tvDialogueLog)
        tvToolCallBanner = findViewById(R.id.tvToolCallBanner)
        scrollView = findViewById(R.id.scrollView)
        btnBargeIn = findViewById(R.id.btnBargeIn)
        btnNavPrompt = findViewById(R.id.btnNavPrompt)
        btnAcPrompt = findViewById(R.id.btnAcPrompt)
        btnSkyPrompt = findViewById(R.id.btnSkyPrompt)
        etCustomQuery = findViewById(R.id.etCustomQuery)
        btnVoice = findViewById(R.id.btnVoice)
        btnSend = findViewById(R.id.btnSend)
        initSpeechRecognizer()

        btnVoice.setOnClickListener {
            startSpeechRecognition()
        }
        btnBargeIn.setOnClickListener {
            agentManager.triggerBargeIn()
            appendLog("[用户] ⚡ 手动触发 Barge-in 硬件级打断！")
        }

        // Quick simulation prompts matching Section 5.2
        btnNavPrompt.setOnClickListener {
            val q = "帮我把导航设到杭州西湖文化广场"
            appendLog("[用户问] $q")
            agentManager.submitUserQuery(q)
        }

        btnAcPrompt.setOnClickListener {
            val q = "把空调温度调到二十四度"
            appendLog("[用户问] $q")
            agentManager.submitUserQuery(q)
        }

        btnSkyPrompt.setOnClickListener {
            val q = "为什么天空是蓝色的啊"
            appendLog("[用户问] $q")
            agentManager.submitUserQuery(q)
        }

        // Custom text chat input
        btnSend.setOnClickListener {
            val text = etCustomQuery.text.toString().trim()
            if (text.isNotEmpty()) {
                appendLog("[用户问] $text")
                agentManager.submitUserQuery(text)
                etCustomQuery.setText("")
            }
        }
    }

    private fun initSpeechRecognizer() {
        if (android.speech.SpeechRecognizer.isRecognitionAvailable(this)) {
            speechRecognizer = android.speech.SpeechRecognizer.createSpeechRecognizer(this).apply {
                setRecognitionListener(object : android.speech.RecognitionListener {
                    override fun onReadyForSpeech(params: Bundle?) {
                        appendLog("[语音识别] 正在聆听，请对着麦克风说话...")
                        btnVoice.text = "🔴 聆听中"
                    }
                    override fun onBeginningOfSpeech() {}
                    override fun onRmsChanged(rmsdB: Float) {}
                    override fun onBufferReceived(buffer: ByteArray?) {}
                    override fun onEndOfSpeech() {
                        btnVoice.text = "🎙️ 说话"
                    }
                    override fun onError(error: Int) {
                        btnVoice.text = "🎙️ 说话"
                    }
                    override fun onResults(results: Bundle?) {
                        btnVoice.text = "🎙️ 说话"
                        val matches = results?.getStringArrayList(android.speech.SpeechRecognizer.RESULTS_RECOGNITION)
                        val text = matches?.firstOrNull()
                        if (!text.isNullOrEmpty()) {
                            etCustomQuery.setText(text)
                            appendLog("[用户说] $text")
                            agentManager.submitUserQuery(text)
                        }
                    }
                    override fun onPartialResults(partialResults: Bundle?) {
                        val matches = partialResults?.getStringArrayList(android.speech.SpeechRecognizer.RESULTS_RECOGNITION)
                        matches?.firstOrNull()?.let {
                            appendLog("[识别增量] $it")
                        }
                    }
                    override fun onEvent(eventType: Int, params: Bundle?) {}
                })
            }
        }
    }

    private fun startSpeechRecognition() {
        if (speechRecognizer != null) {
            val intent = android.content.Intent(android.speech.RecognizerIntent.ACTION_RECOGNIZE_SPEECH).apply {
                putExtra(android.speech.RecognizerIntent.EXTRA_LANGUAGE_MODEL, android.speech.RecognizerIntent.LANGUAGE_MODEL_FREE_FORM)
                putExtra(android.speech.RecognizerIntent.EXTRA_LANGUAGE, "zh-CN")
                putExtra(android.speech.RecognizerIntent.EXTRA_PARTIAL_RESULTS, true)
            }
            speechRecognizer?.startListening(intent)
        } else {
            android.widget.Toast.makeText(this, "语音识别服务就绪中，也可直接打字发送", android.widget.Toast.LENGTH_SHORT).show()
        }
    }

    private fun initAgent() {
        agentManager = AudioAgentManager.getInstance(this)
        agentManager.addListener(this)
        agentManager.addToolListener { result ->
            runOnUiThread {
                handleToolExecutionResult(result)
            }
        }

        val cores = agentManager.getPerformanceCores()
        tvCpuInfo.text = "CPU: 核心 ${cores.joinToString(",")} 绑定 | 正常"
    }

    private fun checkPermissionsAndStart() {
        val permissions = mutableListOf(Manifest.permission.RECORD_AUDIO)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            permissions.add(Manifest.permission.POST_NOTIFICATIONS)
        }

        val needed = permissions.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }

        if (needed.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, needed.toTypedArray(), 100)
        } else {
            startEngine()
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        startEngine()
    }

    private fun startEngine() {
        AudioAgentService.startService(this)
        agentManager.initializeAndStart()
    }

    private fun appendLog(msg: String) {
        android.util.Log.i("MainActivity", msg)
        tvDialogueLog.append("$msg\n")
        scrollView.post {
            scrollView.fullScroll(ScrollView.FOCUS_DOWN)
        }
    }

    private fun handleToolExecutionResult(result: ToolExecutionResult) {
        tvToolCallBanner.visibility = View.VISIBLE
        tvToolCallBanner.text = "⚡ 异步工具执行完成: ${result.summary}\nJSON: ${result.rawJson}"
        appendLog("[异步工具执行] ${result.summary}")
    }

    // -------------------------------------------------------------
    // AudioAgentListener Implementation
    // -------------------------------------------------------------

    override fun onStateChanged(newState: AgentState, oldState: AgentState) {
        runOnUiThread {
            stateVisualizerView.setState(newState)
            tvCurrentState.text = "当前状态: ${newState.displayName}"
            appendLog("[状态机迁移] ${oldState.name} ➔ ${newState.name}")

            // Simulate waveform activity during speaking/listening
            val dummyPcm = FloatArray(128) { (Math.random().toFloat() - 0.5f) * 0.8f }
            audioWaveformView.updateAmplitudes(dummyPcm)
        }
    }

    override fun onAsrPartial(partialText: String) {
        runOnUiThread {
            appendLog("[ASR 流式增量] $partialText")
        }
    }

    override fun onAsrFinal(finalText: String) {
        runOnUiThread {
            appendLog("[ASR 尾点定字] $finalText")
        }
    }

    override fun onTtsSentenceStart(sentence: String) {
        runOnUiThread {
            appendLog("[TTS 首句播报] $sentence")
        }
    }

    override fun onToolCall(toolJson: String) {
        runOnUiThread {
            appendLog("[拦截工具标签] 识别到 <|tool_call|>，不送入TTS，分流执行。")
        }
    }

    override fun onBargeIn(spokenTextRetained: String) {
        runOnUiThread {
            appendLog("⚠️ [Barge-in 打断生效] 喇叭瞬间静音，已播报内容对齐记忆: \"$spokenTextRetained\"")
        }
    }

    override fun onWatchdogFallback(reason: String) {
        runOnUiThread {
            appendLog("🛡️ [看门狗兜底触发] 原因: $reason")
        }
    }

    override fun onDestroy() {
        speechRecognizer?.destroy()
        agentManager.removeListener(this)
        super.onDestroy()
    }
}

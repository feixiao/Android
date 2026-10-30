package com.ai.audioagent

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.View
import android.widget.Button
import android.widget.ScrollView
import android.widget.TextView
import android.widget.ProgressBar
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.launch
import com.ai.audioagent.manager.ModelManager
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
    private lateinit var btnWhoPrompt: Button
    private lateinit var btnWeatherPrompt: Button
    private lateinit var btnNavPrompt: Button
    private lateinit var btnMusicPrompt: Button
    private lateinit var btnAcPrompt: Button
    private lateinit var btnJokePrompt: Button
    private lateinit var btnSkyPrompt: Button
    private lateinit var btnTimePrompt: Button
    private lateinit var etCustomQuery: android.widget.EditText
    private lateinit var btnVoice: Button
    private lateinit var btnSend: Button
    private lateinit var btnModelAction: Button
    private lateinit var tvModelStatus: TextView
    private lateinit var pbModelDownload: ProgressBar
    private lateinit var btnSwitchLlmMode: Button
    private lateinit var tvLlmTelemetry: TextView
    private var currentLlmMode = 0 // 0 = Local MNN-LLM, 1 = Cloud API Mock
    private lateinit var btnDownloadLlm: Button
    private lateinit var tvLlmWeightStatus: TextView
    private lateinit var pbLlmDownload: ProgressBar
    private var isDownloadingLlm = false
    private var isDownloadingModel = false
    private var speechRecognizer: android.speech.SpeechRecognizer? = null
    private var isListeningSpeech = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        agentManager = AudioAgentManager.getInstance(this)
        initViews()
        initAgent()
        checkPermissionsAndStart()
        handleIntent(intent)
    }

    override fun onNewIntent(intent: android.content.Intent?) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    private fun handleIntent(intent: android.content.Intent?) {
        if (intent == null) return

        // 1. DeepLink 统一协议解析: voiceagent://query?text=... (Section 3.3)
        val dataUri = intent.data
        if (dataUri != null && dataUri.scheme == "voiceagent") {
            val deepQuery = dataUri.getQueryParameter("text")
                ?: dataUri.getQueryParameter("query")
                ?: dataUri.getQueryParameter("q")
            if (!deepQuery.isNullOrEmpty()) {
                val source = dataUri.getQueryParameter("source") ?: "DeepLink"
                appendLog("[外部接入] 🔗 DeepLink 意图注入 ($source): $deepQuery")
                agentManager.submitUserQuery(deepQuery)
                return
            }
        }

        // 2. 标准 Intent 参数注入
        intent.getStringExtra("query")?.let { q ->
            if (q.isNotEmpty()) {
                appendLog("[外部接入] 📩 Intent 意图注入: $q")
                agentManager.submitUserQuery(q)
            }
        }
        if (intent.getBooleanExtra("start_voice", false)) {
            startSpeechRecognition()
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
        btnWhoPrompt = findViewById(R.id.btnWhoPrompt)
        btnWeatherPrompt = findViewById(R.id.btnWeatherPrompt)
        btnNavPrompt = findViewById(R.id.btnNavPrompt)
        btnMusicPrompt = findViewById(R.id.btnMusicPrompt)
        btnAcPrompt = findViewById(R.id.btnAcPrompt)
        btnJokePrompt = findViewById(R.id.btnJokePrompt)
        btnSkyPrompt = findViewById(R.id.btnSkyPrompt)
        btnTimePrompt = findViewById(R.id.btnTimePrompt)
        etCustomQuery = findViewById(R.id.etCustomQuery)
        btnVoice = findViewById(R.id.btnVoice)
        btnSend = findViewById(R.id.btnSend)
        btnModelAction = findViewById(R.id.btnModelAction)
        tvModelStatus = findViewById(R.id.tvModelStatus)
        pbModelDownload = findViewById(R.id.pbModelDownload)
        btnSwitchLlmMode = findViewById(R.id.btnSwitchLlmMode)
        tvLlmTelemetry = findViewById(R.id.tvLlmTelemetry)
        btnDownloadLlm = findViewById(R.id.btnDownloadLlm)
        tvLlmWeightStatus = findViewById(R.id.tvLlmWeightStatus)
        pbLlmDownload = findViewById(R.id.pbLlmDownload)
        updateLlmStatusUI()

        btnDownloadLlm.setOnClickListener {
            if (isDownloadingLlm) return@setOnClickListener

            if (ModelManager.isLlmModelReady(this)) {
                val deleted = ModelManager.deleteLlmModel(this)
                if (deleted) {
                    appendLog("[模型管理] 🗑️ 已删除本地 Qwen-0.5B MNN 真实大模型权重")
                    agentManager.setLlmModelPath("")
                }
                updateLlmStatusUI()
            } else {
                startLlmModelDownload()
            }
        }

        btnSwitchLlmMode.setOnClickListener {
            currentLlmMode = if (currentLlmMode == 0) 1 else 0
            agentManager.setLlmEngineMode(currentLlmMode)
            if (currentLlmMode == 0) {
                btnSwitchLlmMode.text = "⚡ 引擎: 本地 MNN-LLM"
                btnSwitchLlmMode.setTextColor(ContextCompat.getColor(this, R.color.primary_accent))
                appendLog("[引擎切换] ⚡ 已切换至端侧本地 MNN-LLM (Prefix KV Cache 固化 / 零网络依赖)")
            } else {
                btnSwitchLlmMode.text = "☁️ 引擎: 云端 API Mock"
                btnSwitchLlmMode.setTextColor(ContextCompat.getColor(this, R.color.text_secondary))
                appendLog("[引擎切换] ☁️ 已切换至云端 API Mock (模拟 WAN 网络 RTT ~280ms / SSE 分包)")
            }
        }
        updateModelStatusUI()

        btnModelAction.setOnClickListener {
            if (isDownloadingModel) return@setOnClickListener

            if (ModelManager.isModelReady(this)) {
                val deleted = ModelManager.deleteModel(this)
                if (deleted) {
                    appendLog("[模型管理] 🗑️ 已删除本地 Sherpa-onnx 离线模型文件")
                    agentManager.setModelPath("")
                }
                updateModelStatusUI()
            } else {
                startModelDownload()
            }
        }

        initSpeechRecognizer()

        btnVoice.setOnClickListener {
            val customText = etCustomQuery.text.toString().trim()
            if (customText.isNotEmpty()) {
                appendLog("[用户问] $customText")
                agentManager.submitUserQuery(customText)
                etCustomQuery.setText("")
                return@setOnClickListener
            }
            if (ModelManager.isModelReady(this)) {
                appendLog("[全双工免触] 离线 ASR 模型已挂载运行，全双工麦克风处于实时监听状态，请直接对着手机说话，无需按键！")
                return@setOnClickListener
            }
            if (isListeningSpeech) {
                stopSpeechRecognition()
            } else {
                startSpeechRecognition()
            }
        }
        btnBargeIn.setOnClickListener {
            agentManager.triggerBargeIn()
            appendLog("[用户] ⚡ 手动触发 Barge-in 硬件级打断！")
        }

        // Rich quick dialogue prompts
        btnWhoPrompt.setOnClickListener {
            val q = "你好，请问你是谁？"
            appendLog("[用户问] $q")
            agentManager.submitUserQuery(q)
        }
        btnWeatherPrompt.setOnClickListener {
            val q = "今天外面天气怎么样？"
            appendLog("[用户问] $q")
            agentManager.submitUserQuery(q)
        }
        btnNavPrompt.setOnClickListener {
            val q = "帮我把导航设到杭州西湖文化广场"
            appendLog("[用户问] $q")
            agentManager.submitUserQuery(q)
        }
        btnMusicPrompt.setOnClickListener {
            val q = "放一首周杰伦的晴天"
            appendLog("[用户问] $q")
            agentManager.submitUserQuery(q)
        }
        btnAcPrompt.setOnClickListener {
            val q = "把空调温度调到二十四度"
            appendLog("[用户问] $q")
            agentManager.submitUserQuery(q)
        }
        btnJokePrompt.setOnClickListener {
            val q = "给我讲个好笑的笑话吧"
            appendLog("[用户问] $q")
            agentManager.submitUserQuery(q)
        }
        btnSkyPrompt.setOnClickListener {
            val q = "为什么天空是蓝色的啊"
            appendLog("[用户问] $q")
            agentManager.submitUserQuery(q)
        }
        btnTimePrompt.setOnClickListener {
            val q = "现在几点了？"
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

    private fun updateModelStatusUI() {
        val ready = ModelManager.isModelReady(this)
        if (ready) {
            val dir = ModelManager.getModelDir(this)
            tvModelStatus.text = "状态: ✅ 离线模型已就绪 (Sherpa-onnx Zipformer)"
            btnModelAction.text = "🗑️ 删除模型"
            btnModelAction.isEnabled = true
            pbModelDownload.visibility = View.GONE
            if (::agentManager.isInitialized) {
                agentManager.setModelPath(dir.absolutePath)
            }
        } else {
            tvModelStatus.text = "状态: ❌ 未安装离线模型 (当前处于轻量参考模式)"
            btnModelAction.text = "📥 下载模型 (31MB)"
            btnModelAction.isEnabled = true
            pbModelDownload.visibility = View.GONE
            if (::agentManager.isInitialized) {
                agentManager.setModelPath("")
            }
        }
    }

    private fun startModelDownload() {
        isDownloadingModel = true
        btnModelAction.isEnabled = false
        btnModelAction.text = "⏳ 下载中..."
        pbModelDownload.visibility = View.VISIBLE
        pbModelDownload.progress = 0
        appendLog("[模型管理] 🚀 开始下载端侧 Sherpa-onnx 离线 ASR 模型 (共 4 个权重文件，约 31MB)...")

        lifecycleScope.launch {
            ModelManager.downloadModels(
                context = this@MainActivity,
                onProgress = { pct, statusText ->
                    runOnUiThread {
                        pbModelDownload.progress = pct
                        tvModelStatus.text = "状态: $statusText"
                    }
                },
                onComplete = { success, message ->
                    runOnUiThread {
                        isDownloadingModel = false
                        updateModelStatusUI()
                        if (success) {
                            appendLog("[模型管理] ✅ $message")
                            android.widget.Toast.makeText(
                                this@MainActivity,
                                "离线模型就绪！支持免触语音识别全双工对聊",
                                android.widget.Toast.LENGTH_SHORT
                            ).show()
                        } else {
                            btnModelAction.text = "📥 重试下载 (31MB)"
                            appendLog("[模型管理] ❌ $message")
                            android.widget.Toast.makeText(
                                this@MainActivity,
                                message,
                                android.widget.Toast.LENGTH_LONG
                            ).show()
                        }
                    }
                }
            )
        }
    }

    private fun updateLlmStatusUI() {
        val ready = ModelManager.isLlmModelReady(this)
        if (ready) {
            val dir = ModelManager.getLlmModelDir(this)
            tvLlmWeightStatus.text = "状态: ✅ 真实Qwen-0.5B已就绪"
            btnDownloadLlm.text = "🗑️ 删除大模型"
            btnDownloadLlm.isEnabled = true
            pbLlmDownload.visibility = View.GONE
            if (::agentManager.isInitialized) {
                agentManager.setLlmModelPath(dir.absolutePath)
            }
        } else {
            tvLlmWeightStatus.text = "状态: ❌ 未就绪 (轻量仿真模式)"
            btnDownloadLlm.text = "📥 下载真实大模型 (350MB)"
            btnDownloadLlm.isEnabled = true
            pbLlmDownload.visibility = View.GONE
            if (::agentManager.isInitialized) {
                agentManager.setLlmModelPath("")
            }
        }
    }

    private fun startLlmModelDownload() {
        isDownloadingLlm = true
        btnDownloadLlm.isEnabled = false
        btnDownloadLlm.text = "⏳ 下载大模型中..."
        pbLlmDownload.visibility = View.VISIBLE
        pbLlmDownload.progress = 0
        appendLog("[模型管理] 🚀 开始下载真实端侧 Qwen2.5-0.5B MNN 模型权重 (约 350MB)...")

        lifecycleScope.launch {
            ModelManager.downloadLlmModels(
                context = this@MainActivity,
                onProgress = { pct, statusText ->
                    runOnUiThread {
                        pbLlmDownload.progress = pct
                        tvLlmWeightStatus.text = "状态: $statusText"
                    }
                },
                onComplete = { success, message ->
                    runOnUiThread {
                        isDownloadingLlm = false
                        updateLlmStatusUI()
                        if (success) {
                            appendLog("[模型管理] ✅ $message")
                            android.widget.Toast.makeText(
                                this@MainActivity,
                                "真实端侧 Qwen-0.5B 大模型已就绪并加载！",
                                android.widget.Toast.LENGTH_SHORT
                            ).show()
                        } else {
                            btnDownloadLlm.text = "📥 重试下载 (350MB)"
                            appendLog("[模型管理] ❌ $message")
                            android.widget.Toast.makeText(
                                this@MainActivity,
                                message,
                                android.widget.Toast.LENGTH_LONG
                            ).show()
                        }
                    }
                }
            )
        }
    }

    private fun initSpeechRecognizer() {
        if (android.speech.SpeechRecognizer.isRecognitionAvailable(this)) {
            speechRecognizer = android.speech.SpeechRecognizer.createSpeechRecognizer(this).apply {
                setRecognitionListener(object : android.speech.RecognitionListener {
                    override fun onReadyForSpeech(params: Bundle?) {
                        appendLog("[语音识别] 麦克风已就绪，正在倾听，请对准麦克风说话...")
                        btnVoice.text = "🔴 请说话"
                    }
                    override fun onBeginningOfSpeech() {
                        btnVoice.text = "🔴 识别中..."
                    }
                    override fun onRmsChanged(rmsdB: Float) {}
                    override fun onBufferReceived(buffer: ByteArray?) {}
                    override fun onEndOfSpeech() {
                        btnVoice.text = "⏳ 解析中..."
                    }
                    override fun onError(error: Int) {
                        isListeningSpeech = false
                        btnVoice.text = "🎙️ 说话"
                        agentManager.resumeAudioIO()
                        val errorMsg = when (error) {
                            android.speech.SpeechRecognizer.ERROR_NETWORK,
                            android.speech.SpeechRecognizer.ERROR_NETWORK_TIMEOUT -> "系统语音网络未连接"
                            android.speech.SpeechRecognizer.ERROR_NO_MATCH -> "未匹配到清晰语音"
                            android.speech.SpeechRecognizer.ERROR_SPEECH_TIMEOUT -> "未检测到说话声音"
                            12, 13 -> "离线中文语音包未就绪"
                            else -> "识别状态码: $error"
                        }
                        appendLog("[语音提示] $errorMsg。请再次点击「🎙️ 说话」对聊，或直接在输入框打字发送！")
                    }
                    override fun onResults(results: Bundle?) {
                        isListeningSpeech = false
                        btnVoice.text = "🎙️ 说话"
                        agentManager.resumeAudioIO()
                        val matches = results?.getStringArrayList(android.speech.SpeechRecognizer.RESULTS_RECOGNITION)
                        val text = matches?.firstOrNull()
                        if (!text.isNullOrEmpty()) {
                            etCustomQuery.setText(text)
                            appendLog("[用户说] $text")
                            agentManager.submitUserQuery(text)
                            etCustomQuery.setText("")
                        } else {
                            appendLog("[语音识别] 未识别到有效文字，请重试或打字发送。")
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
            isListeningSpeech = true
            agentManager.pauseAudioIO()
            btnVoice.text = "🔴 准备中"
            val intent = android.content.Intent(android.speech.RecognizerIntent.ACTION_RECOGNIZE_SPEECH).apply {
                putExtra(android.speech.RecognizerIntent.EXTRA_LANGUAGE_MODEL, android.speech.RecognizerIntent.LANGUAGE_MODEL_FREE_FORM)
                putExtra(android.speech.RecognizerIntent.EXTRA_LANGUAGE, "zh-CN")
                putExtra(android.speech.RecognizerIntent.EXTRA_PARTIAL_RESULTS, true)
            }
            try {
                speechRecognizer?.startListening(intent)
            } catch (e: Exception) {
                isListeningSpeech = false
                agentManager.resumeAudioIO()
                btnVoice.text = "🎙️ 说话"
                appendLog("[语音识别异常] ${e.message}")
            }
        } else {
            android.widget.Toast.makeText(this, "语音识别服务就绪中，也可直接打字发送", android.widget.Toast.LENGTH_SHORT).show()
        }
    }

    private fun stopSpeechRecognition() {
        if (isListeningSpeech) {
            isListeningSpeech = false
            try {
                speechRecognizer?.stopListening()
            } catch (e: Exception) {}
            btnVoice.text = "🎙️ 说话"
            agentManager.resumeAudioIO()
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
        tvToolCallBanner.text = "⚡ 外部工具执行: ${result.summary}\n【调用通道: ${result.channel}】\nJSON: ${result.rawJson}"
        appendLog("[工具调用完成] ${result.summary} (通道: ${result.channel})")
    }

    // -------------------------------------------------------------
    // AudioAgentListener Implementation
    // -------------------------------------------------------------

    override fun onStateChanged(newState: AgentState, oldState: AgentState) {
        runOnUiThread {
            stateVisualizerView.setState(newState)
            tvCurrentState.text = "当前状态: ${newState.displayName}"
            appendLog("[状态机迁移] ${oldState.name} ➔ ${newState.name}")

            if (oldState == AgentState.LISTENING && newState == AgentState.IDLE) {
                if (ModelManager.isModelReady(this@MainActivity)) {
                    appendLog("[麦克风感知] 监听落音截断，全双工持续免触收听中。")
                } else {
                    appendLog("[麦克风感知] 听到语音输入并落音截断。由于未安装 31MB 离线 ASR 模型，请点击上方按钮下载开启免触对聊，或直接打字发送！")
                }
            }
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
            appendLog("[用户说] $finalText")
        }
    }

    override fun onTtsSentenceStart(sentence: String) {
        runOnUiThread {
            appendLog("[TTS 首句播报] $sentence")
        }
    }

    override fun onToolCall(toolJson: String) {
        runOnUiThread {
            appendLog("[工具指令分流] 捕获控制指令: $toolJson，不送入TTS，静默分流执行")
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

    override fun onLlmStats(statsJson: String) {
        try {
            val json = org.json.JSONObject(statsJson)
            val engine = json.optString("engine", "MNN-LLM")
            val ttft = json.optLong("ttft_ms")
            val prefill = json.optLong("prefill_ms")
            val tps = json.optDouble("tps", 0.0)
            val kvTokens = json.optLong("kv_tokens")
            val prefixHit = json.optBoolean("prefix_hit")

            runOnUiThread {
                val hitText = if (prefixHit) "命中 (增量Prefill)" else "未命中 (全量Prefill)"
                tvLlmTelemetry.text = "遥测: 引擎: $engine | Prefix: $hitText (${prefill}ms) | TTFT: ${ttft}ms | 解码: ${"%.1f".format(tps)} tok/s | KV Tokens: $kvTokens"
                appendLog("[端侧 LLM 遥测] 引擎: $engine | Prefix: $hitText (${prefill}ms) | TTFT: ${ttft}ms | 解码: ${"%.1f".format(tps)} tok/s | KV: $kvTokens Tokens")
            }
        } catch (e: Exception) {
            android.util.Log.e("MainActivity", "Failed to parse LLM stats", e)
        }
    }

    override fun onPause() {
        super.onPause()
        stopSpeechRecognition()
    }

    override fun onDestroy() {
        speechRecognizer?.destroy()
        agentManager.removeListener(this)
        super.onDestroy()
    }
}

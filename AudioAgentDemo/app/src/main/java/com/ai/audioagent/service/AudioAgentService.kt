package com.ai.audioagent.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import com.ai.audioagent.MainActivity
import com.ai.audioagent.manager.AudioAgentManager
import com.ai.audioagent.nativebridge.AgentState
import com.ai.audioagent.nativebridge.AudioAgentListener
import android.util.Log
/**
 * ==============================================================================================
 *                     AudioAgentService 核心实现架构与前台保活设计
 * ==============================================================================================
 *
 * 【系统架构分层拓扑图】
 * ┌────────────────────────────────────────────────────────────────────────────────────────┐
 * │                              Android 宿主应用层 (Application / UI)                     │
 * │                                                                                        │
 * │   [MainActivity] ──► StateVisualizerView (光环渲染) & AudioWaveformView (波形频谱)     │
 * │          │                                                              ▲              │
 * │          │ startService(ACTION_START)                                   │ 状态/波形分发│
 * │          ▼                                                              │              │
 * │ ┌─────────────────────────────────────────────────────────────────────────────┐        │
 * │ │                     AudioAgentService (前台保活常驻服务)                     │        │
 * │ │                                                                             │        │
 * │ │  - 声明类型: foregroundServiceType="microphone|mediaPlayback" (防 lmkd 强杀) │        │
 * │ │  - 意图路由 (onStartCommand):                                               │        │
 * │ │    * ACTION_START    ──► startForeground() + agentManager.initializeAndStart()│        │
 * │ │    * ACTION_STOP     ──► agentManager.stop() + stopForeground() + stopSelf()  │        │
 * │ │    * ACTION_BARGE_IN ──► agentManager.triggerBargeIn() (微秒级打断)          │        │
 * │ │                                                                             │        │
 * │ │  - 常驻通知栏联动 (NotificationChannel: IMPORTANCE_LOW 静音免打扰):          │        │
 * │ │    * PendingIntent_Activity ──► 快捷回退至 MainActivity (SingleTop)         │        │
 * │ │    * PendingIntent_Service  ──► 常驻 [打断 (Barge-in)] 快捷操作按钮         │        │
 * │ │    * onStateChanged 回调    ──► 动态更新副标题: IDLE/LISTENING/THINKING/SPEAKING     │
 * │ └──────────────────────────────────────┬──────────────────────────────────────┘        │
 * │                                        │ 控制调用 & 事件监听                           │
 * │                                        ▼                                               │
 * │ ┌─────────────────────────────────────────────────────────────────────────────┐        │
 * │ │                       AudioAgentManager (中枢调度器 单例)                   │        │
 * │ │                                                                             │        │
 * │ │  - 音频焦点: requestAudioFocus (VOICE_COMMUNICATION, 电话呼入瞬时降权)      │        │
 * │ │  - 温控感知: PowerManager.OnThermalStatusChangedListener ──► JNI 同步       │        │
 * │ │  - 工具分发: SystemToolExecutor ──► 拦截并执行空调/闹钟/导航异步任务        │        │
 * │ │  - 本地模型探测: ModelManager 校验 ASR / LLM 权重目录可用性                 │        │
 * │ └──────────────────────────────────────┬──────────────────────────────────────┘        │
 * └────────────────────────────────────────┼───────────────────────────────────────────────┘
 *                                          │ 调用 / JNI 回调
 *                                          ▼
 * ┌────────────────────────────────────────────────────────────────────────────────────────┐
 * │                     JNI 胶水层 (AudioAgentNative.kt & NativeJniBridge.cpp)             │
 * │                                                                                        │
 * │  - 线程环境附加: JavaVM::AttachCurrentThread(&env, nullptr) + DetachCurrentThread()    │
 * │  - 异常安全防护: env->ExceptionCheck() -> env->ExceptionDescribe() -> Clear()          │
 * │  - 双向接口桥接: nativeStart / nativeStop / nativeTriggerBargeIn / nativeSubmitUserQuery│
 * └────────────────────────────────────────┬───────────────────────────────────────────────┘
 *                                          │ 控制流与数据流
 *                                          ▼
 * ┌────────────────────────────────────────────────────────────────────────────────────────┐
 * │                      ISO C++17 全双工核心引擎层 (libaudio_agent.so)                    │
 * │                                                                                        │
 * │  - AudioAgentPipeline (全双工 FSM 状态机: IDLE / LISTENING / THINKING / SPEAKING)      │
 * │  - Oboe 硬件双向流 (VoiceCommunication 模式, MMAP Shared, 芯片 DSP AEC 硬件回声消除)    │
 * │  - 感知线程 (Sherpa-onnx VAD + Zipformer ASR, 能效小核绑定, 功耗 <150mW)               │
 * │  - 认知线程 (MNN-LLM Qwen2.5 + Prefix KV Cache 固化 + 首句动态分块切句器)              │
 * │  - 表达线程 (DemoTtsEngine / Oboe 24kHz 输出流)                                        │
 * │  - Barge-in 打断协调器 (<5ms 极速终止解码, SPSC 缓冲排空, KV Cache 回滚至实际播报字数)  │
 * │  - 闲时监控线程 (ThreeTierMemory L2 语义记忆提炼与本地持久化)                          │
 * └────────────────────────────────────────────────────────────────────────────────────────┘
 *
 * 【核心交互时序】
 * 1. 启动与前台提权:
 *    MainActivity -> startForegroundService(ACTION_START)
 *      -> AudioAgentService.startForeground(MICROPHONE | MEDIA_PLAYBACK) [提权防 lmkd 强杀]
 *      -> AudioAgentManager.initializeAndStart() -> AudioAgentNative.start() -> C++ AudioAgentPipeline
 *
 * 2. 状态监听与通知联动闭环:
 *    C++ 工作线程 -> onStateChanged -> NativeJniBridge.cpp -> AudioAgentNative.kt
 *      -> AudioAgentManager -> AudioAgentService.onStateChanged() -> NotificationManager.notify()
 *      -> 动态刷新常驻通知副标题 (IDLE / LISTENING / THINKING / SPEAKING)
 *
 * 3. 锁屏/通知栏一键打断 (Barge-in):
 *    用户点击通知栏 "[打断 (Barge-in)]" 按钮 -> PendingIntent 发送 ACTION_BARGE_IN
 *      -> AudioAgentService.onStartCommand -> AudioAgentManager.triggerBargeIn()
 *      -> AudioAgentNative.triggerBargeIn() -> C++ pipeline->TriggerBargeIn()
 *      -> <5ms 硬件打断，排空播放缓冲并回滚大模型 KV Cache 记忆
 */
class AudioAgentService : Service(), AudioAgentListener {

    /** 全局核心管理器单例引用，用于调度底层流水线与上层系统特性 */
    private lateinit var agentManager: AudioAgentManager

    /** 前台常驻通知唯一 ID (同一服务生命周期内固定不变，保证通知唯一更新) */
    private val notificationId = 1001

    /** Android 8.0+ 系统通知渠道唯一标识符 */
    private val channelId = "audio_agent_service_channel"

    /** 跨进程客户端监听器列表 (RemoteCallbackList 自动处理进程消亡清理) */
    private val remoteCallbacks = android.os.RemoteCallbackList<com.ai.audioagent.ipc.IAudioAgentCallback>()

    /**
     * 跨进程 AIDL Binder 服务端桩对象 (IPC Stub)
     * 供系统 UI、桌面 Launcher、第三方地图/音乐等独立进程远程绑定调用
     */
    private val binder = object : com.ai.audioagent.ipc.IAudioAgentService.Stub() {
        override fun submitUserQuery(query: String?, extras: android.os.Bundle?) {
            if (!query.isNullOrEmpty()) {
                Log.i(TAG, "IPC Received submitUserQuery: '$query'")
                agentManager.submitUserQuery(query)
            }
        }

        override fun triggerBargeIn() {
            Log.i(TAG, "IPC Received triggerBargeIn")
            agentManager.triggerBargeIn()
        }

        override fun pauseAudioCapture() {
            Log.i(TAG, "IPC Received pauseAudioCapture")
            agentManager.pauseAudioIO()
        }

        override fun resumeAudioCapture(): Boolean {
            Log.i(TAG, "IPC Received resumeAudioCapture")
            return agentManager.resumeAudioIO()
        }

        override fun registerCallback(callback: com.ai.audioagent.ipc.IAudioAgentCallback?) {
            if (callback != null) {
                remoteCallbacks.register(callback)
                Log.i(TAG, "Registered remote IPC callback (Total: ${remoteCallbacks.registeredCallbackCount})")
            }
        }

        override fun unregisterCallback(callback: com.ai.audioagent.ipc.IAudioAgentCallback?) {
            if (callback != null) {
                remoteCallbacks.unregister(callback)
                Log.i(TAG, "Unregistered remote IPC callback (Total: ${remoteCallbacks.registeredCallbackCount})")
            }
        }
    }

    /** 工具执行结果监听器闭包引用，便于 onDestroy 时精确释放 */
    private val toolResultListener: (com.ai.audioagent.tools.ToolExecutionResult) -> Unit = { result ->
        dispatchToolExecutedToRemote(result.action, result.success, result.summary)
    }

    /**
     * @brief 服务初始化生命周期
     *
     * 执行流程：
     * 1. super.onCreate() 初始化系统服务基类；
     * 2. 获取 AudioAgentManager 单例并注册当前服务为状态监听器，以便感知底层 FSM 状态；
     * 3. 注册工具执行监听器，用于将高德导航等工具结果同步给 IPC 远程调用方；
     * 4. 在 Android 8.0+ 设备上预先创建低优先级静音通知渠道 (NotificationChannel)。
     */
    override fun onCreate() {
        super.onCreate()
        // 获取中枢调度器单例实例
        agentManager = AudioAgentManager.getInstance(this)
        // 注册当前前台服务为事件监听者，用于感知底层状态流转
        agentManager.addListener(this)
        // 注册工具执行结果监听器，用于跨进程广播
        agentManager.addToolListener(toolResultListener)
        // 创建系统通知渠道（Android 8.0+ 强制要求）
        createNotificationChannel()
    }

    /**
     * @brief 意图命令路由分发中心 (Command Intent Router)
     *
     * 响应外部通过 startService / startForegroundService 发送的控制指令：
     * - ACTION_START: 提权为复合前台服务，并驱动 AudioAgentManager 初始化与启动底层 C++ 全双工流水线；
     * - ACTION_STOP: 停止流水线音频流，释放前台通知凭据并调用 stopSelf 销毁服务；
     * - ACTION_BARGE_IN: 接收来自通知栏快捷操作按钮的打断请求，触发微秒级 Barge-in 中断。
     *
     * @param intent 启动服务时传入的包含 Action 命令的意图对象
     * @param flags 启动附加标志位
     * @param startId 当前请求任务唯一 ID
     * @return 返回 START_STICKY，确保在极端低内存被杀后，系统在内存恢复时自动重建服务保持常驻。
     */
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> {
                // 1. 构建初始待命状态 (IDLE) 的常驻通知
                val notification = buildNotification(AgentState.IDLE)

                // 2. 针对 Android 10 (Q) 及 Android 11 (R)+ / Android 14+ 动态装配复合前台服务类型
                //    - FOREGROUND_SERVICE_TYPE_MICROPHONE: 声明长期占用硬件麦克风（VAD 监听）
                //    - FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK: 声明长期占用扬声器执行流式发声
                //    复合前台服务使得系统分配最高的 oom_adj 优先级（FOREGROUND_APP 级别），免疫 lmkd 杀后台
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    var serviceType = ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                        serviceType = serviceType or ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
                    }
                    startForeground(notificationId, notification, serviceType)
                } else {
                    // Android 9.0 及以下系统无需声明具体 ServiceType
                    startForeground(notificationId, notification)
                }

                // 3. 驱动中枢管理器初始化 Native 引擎、请求音频焦点并启动工作线程池
                agentManager.initializeAndStart()
            }
            ACTION_STOP -> {
                // 1. 停止底层 Oboe 录音/播音流与推理线程池，平滑释放硬件资源
                agentManager.stop()
                // 2. 撤销前台保活凭据，从通知中心移除常驻通知
                stopForeground(STOP_FOREGROUND_REMOVE)
                // 3. 自杀退出服务进程
                stopSelf()
            }
            ACTION_BARGE_IN -> {
                // 收到打断指令，立即通过原子标志位中止大模型解码与 TTS 输出，并回滚未朗读的 KV Cache
                agentManager.triggerBargeIn()
            }
        }
        // 返回 START_STICKY：若服务因系统内存耗尽被杀，当内存缓解后系统会自动重建该 Service
        return START_STICKY
    }

    /**
     * @brief 绑定接口实现：返回跨进程 AIDL Binder 实例
     *
     * 允许外部系统服务或第三方授权应用通过 bindService 跨进程连接本服务，
     * 直接执行 submitUserQuery、triggerBargeIn 或监听全双工事件流。
     */
    override fun onBind(intent: Intent?): IBinder {
        Log.i(TAG, "AudioAgentService bound by client: action=${intent?.action}")
        return binder
    }

    /**
     * @brief 服务销毁生命周期
     *
     * 注销对 AudioAgentManager 的事件监听与工具监听，排空跨进程监听队列，杜绝内存泄漏。
     */
    override fun onDestroy() {
        agentManager.removeListener(this)
        agentManager.removeToolListener(toolResultListener)
        remoteCallbacks.kill()
        Log.i(TAG, "AudioAgentService destroyed and remote callbacks killed.")
        super.onDestroy()
    }

    // -------------------------------------------------------------
    // AudioAgentListener 回调实现：与底层 C++ 状态机及 IPC 闭环联动
    // -------------------------------------------------------------

    /**
     * @brief 全双工状态机 (FSM) 状态流转回调
     *
     * 1. 动态刷新系统前台通知栏副标题；
     * 2. 跨进程广播通知所有绑定的外部应用 (IAudioAgentCallback)。
     */
    override fun onStateChanged(newState: AgentState, oldState: AgentState) {
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager
        // 动态刷新前台常驻通知副标题，告知用户当前 Agent 处于倾听中、思考中或播报中
        manager?.notify(notificationId, buildNotification(newState))

        // 跨进程分发
        val count = remoteCallbacks.beginBroadcast()
        for (i in 0 until count) {
            try {
                remoteCallbacks.getBroadcastItem(i).onStateChanged(newState.ordinal, oldState.ordinal)
            } catch (e: Exception) {
                Log.w(TAG, "Failed to broadcast onStateChanged to client $i", e)
            }
        }
        remoteCallbacks.finishBroadcast()
    }

    /** 增量 ASR 识别中间结果 */
    override fun onAsrPartial(partialText: String) {
        broadcastAsrStreaming(partialText, isFinal = false)
    }

    /** 最终结算 ASR 文本 */
    override fun onAsrFinal(finalText: String) {
        broadcastAsrStreaming(finalText, isFinal = true)
    }

    /** TTS 开始播报短句回调 */
    override fun onTtsSentenceStart(sentence: String) {
        val count = remoteCallbacks.beginBroadcast()
        for (i in 0 until count) {
            try {
                remoteCallbacks.getBroadcastItem(i).onTtsSentenceStart(sentence)
            } catch (e: Exception) {
                Log.w(TAG, "Failed to broadcast onTtsSentenceStart to client $i", e)
            }
        }
        remoteCallbacks.finishBroadcast()
    }

    /** 大模型异步工具调用 JSON 截获 */
    override fun onToolCall(toolJson: String) {}

    /** Barge-in 硬件打断触发通知 */
    override fun onBargeIn(spokenTextRetained: String) {}

    private fun broadcastAsrStreaming(text: String, isFinal: Boolean) {
        val count = remoteCallbacks.beginBroadcast()
        for (i in 0 until count) {
            try {
                remoteCallbacks.getBroadcastItem(i).onAsrStreaming(text, isFinal)
            } catch (e: Exception) {
                Log.w(TAG, "Failed to broadcast onAsrStreaming to client $i", e)
            }
        }
        remoteCallbacks.finishBroadcast()
    }

    private fun dispatchToolExecutedToRemote(action: String, success: Boolean, summary: String) {
        val count = remoteCallbacks.beginBroadcast()
        for (i in 0 until count) {
            try {
                remoteCallbacks.getBroadcastItem(i).onToolExecuted(action, success, summary)
            } catch (e: Exception) {
                Log.w(TAG, "Failed to broadcast onToolExecuted to client $i", e)
            }
        }
        remoteCallbacks.finishBroadcast()
    }
    /** 首字延迟看门狗超时降级通知 */
    override fun onWatchdogFallback(reason: String) {}

    // -------------------------------------------------------------
    // 通知栏与前台服务辅助机制
    // -------------------------------------------------------------

    /**
     * @brief 创建系统通知渠道 (NotificationChannel)
     *
     * 工程安全铁律：
     * 渠道重要性必须配置为 `NotificationManager.IMPORTANCE_LOW`（静音无振动）。
     * 绝对禁止在全双工语音助手运行时发出任何通知提示音，否则该提示音会被硬件麦克风二度捕获，
     * 误触 VAD 人声判定，导致系统进入自身声音打断自身（Self-Barge-in）的恶性死循环。
     */
    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                channelId,
                "AI Audio Agent Service",
                NotificationManager.IMPORTANCE_LOW // 必须为低优先级，杜绝通知发音干扰全双工声学回路
            ).apply {
                description = "全双工端侧 AI 语音常驻服务"
                setShowBadge(false) // 禁用桌面应用图标角标干扰
            }
            val manager = getSystemService(NotificationManager::class.java)
            manager?.createNotificationChannel(channel)
        }
    }

    /**
     * @brief 构建包含实时状态与交互按钮的前台常驻通知
     *
     * 包含四大工程特性：
     * 1. 【SingleTop 栈顶复用】：点击通知主体跳转回 MainActivity 时携带 FLAG_ACTIVITY_SINGLE_TOP，
     *    避免重复重建渲染栈；
     * 2. 【一键打断 Action】：在通知栏右下角附加 "[打断 (Barge-in)]" 快捷操作按钮，
     *    用户无需打开 App 界面，即可在锁屏或下拉通知中心完成语音播报的极速硬掐断；
     * 3. 【FLAG_IMMUTABLE 强制安全规范】：兼容 Android 12+ (API 31+) 强制不可变性要求；
     * 4. 【setOngoing(true)】：声明为常驻不可划除通知，作为前台保活的系统级物理锚点。
     *
     * @param state 当前全双工 Agent 状态
     * @return 组装完成的 Notification 实体
     */
    private fun buildNotification(state: AgentState): Notification {
        // 1. 点击通知主体的回跳意图 (复用已有栈顶 MainActivity)
        val openIntent = Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP // 栈顶复用，防止重复创建视图实例
        }
        val openPendingIntent = PendingIntent.getActivity(
            this,
            0,
            openIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE // 适配 Android 12+ 安全规范
        )

        // 2. 点击通知栏 [打断] 按钮的快捷服务意图 (直接向本 Service 发送 ACTION_BARGE_IN)
        val bargeInIntent = Intent(this, AudioAgentService::class.java).apply {
            action = ACTION_BARGE_IN
        }
        val bargeInPendingIntent = PendingIntent.getService(
            this,
            1,
            bargeInIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val iconRes = android.R.drawable.ic_btn_speak_now

        // 3. 构建前台通知
        return NotificationCompat.Builder(this, channelId)
            .setContentTitle("全双工 AI 语音助手")
            .setContentText("当前状态: ${state.displayName}")
            .setSmallIcon(iconRes)
            .setContentIntent(openPendingIntent)
            .setOngoing(true) // 常驻不可划除，保障系统保活凭据
            .addAction(android.R.drawable.ic_media_pause, "打断 (Barge-in)", bargeInPendingIntent)
            .setPriority(NotificationCompat.PRIORITY_LOW) // 静音免打扰
            .build()
    }

    /**
     * @brief 静态伴生对象：提供控制意图常量与外部快捷启动/停止入口
     */
    companion object {
        private const val TAG = "AudioAgentService"
        /** 启动全双工服务控制动作 (提权前台并启动流水线) */
        const val ACTION_START = "com.ai.audioagent.action.START"

        /** 停止并销毁全双工服务控制动作 (释放硬件并注销通知) */
        const val ACTION_STOP = "com.ai.audioagent.action.STOP"

        /** 外部或通知栏触发微秒级 Barge-in 打断控制动作 */
        const val ACTION_BARGE_IN = "com.ai.audioagent.action.BARGE_IN"

        /**
         * @brief 外部便捷启动常驻服务静态入口
         *
         * 自动向下兼容：Android 8.0+ (API 26+) 使用 startForegroundService，
         * 低版本系统使用常规 startService。
         *
         * @param context 上下文对象
         */
        fun startService(context: Context) {
            val intent = Intent(context, AudioAgentService::class.java).apply {
                action = ACTION_START
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

        /**
         * @brief 外部便捷停止常驻服务静态入口
         *
         * @param context 上下文对象
         */
        fun stopService(context: Context) {
            val intent = Intent(context, AudioAgentService::class.java).apply {
                action = ACTION_STOP
            }
            context.startService(intent)
        }
    }
}

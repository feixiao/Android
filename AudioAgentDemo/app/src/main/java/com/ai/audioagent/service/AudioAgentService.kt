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

/**
 * Android Foreground Service for Audio Agent.
 * Configured with foregroundServiceType="microphone|mediaPlayback" per Whitepaper Section 6.3
 * to defend against Android lmkd (Low Memory Killer Daemon) killing high-memory AI models.
 */
class AudioAgentService : Service(), AudioAgentListener {

    private lateinit var agentManager: AudioAgentManager
    private val notificationId = 1001
    private val channelId = "audio_agent_service_channel"

    override fun onCreate() {
        super.onCreate()
        agentManager = AudioAgentManager.getInstance(this)
        agentManager.addListener(this)
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> {
                val notification = buildNotification(AgentState.IDLE)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    var serviceType = ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                        serviceType = serviceType or ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
                    }
                    startForeground(notificationId, notification, serviceType)
                } else {
                    startForeground(notificationId, notification)
                }
                agentManager.initializeAndStart()
            }
            ACTION_STOP -> {
                agentManager.stop()
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
            }
            ACTION_BARGE_IN -> {
                agentManager.triggerBargeIn()
            }
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        agentManager.removeListener(this)
        super.onDestroy()
    }

    // -------------------------------------------------------------
    // AudioAgentListener
    // -------------------------------------------------------------

    override fun onStateChanged(newState: AgentState, oldState: AgentState) {
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager
        manager?.notify(notificationId, buildNotification(newState))
    }

    override fun onAsrPartial(partialText: String) {}
    override fun onAsrFinal(finalText: String) {}
    override fun onTtsSentenceStart(sentence: String) {}
    override fun onToolCall(toolJson: String) {}
    override fun onBargeIn(spokenTextRetained: String) {}
    override fun onWatchdogFallback(reason: String) {}

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                channelId,
                "AI Audio Agent Service",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "全双工端侧 AI 语音常驻服务"
                setShowBadge(false)
            }
            val manager = getSystemService(NotificationManager::class.java)
            manager?.createNotificationChannel(channel)
        }
    }

    private fun buildNotification(state: AgentState): Notification {
        val openIntent = Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP
        }
        val openPendingIntent = PendingIntent.getActivity(
            this,
            0,
            openIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

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

        return NotificationCompat.Builder(this, channelId)
            .setContentTitle("全双工 AI 语音助手")
            .setContentText("当前状态: ${state.displayName}")
            .setSmallIcon(iconRes)
            .setContentIntent(openPendingIntent)
            .setOngoing(true)
            .addAction(android.R.drawable.ic_media_pause, "打断 (Barge-in)", bargeInPendingIntent)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
    }

    companion object {
        const val ACTION_START = "com.ai.audioagent.action.START"
        const val ACTION_STOP = "com.ai.audioagent.action.STOP"
        const val ACTION_BARGE_IN = "com.ai.audioagent.action.BARGE_IN"

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

        fun stopService(context: Context) {
            val intent = Intent(context, AudioAgentService::class.java).apply {
                action = ACTION_STOP
            }
            context.startService(intent)
        }
    }
}

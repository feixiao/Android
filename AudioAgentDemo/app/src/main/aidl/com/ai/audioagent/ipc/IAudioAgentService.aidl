package com.ai.audioagent.ipc;

import com.ai.audioagent.ipc.IAudioAgentCallback;
import android.os.Bundle;

/**
 * 跨进程 AudioAgent 常驻服务控制接口 (IPC Service)
 * 支持整机各独立进程 (Launcher、SystemUI、第三方生态 App) 远程连接与控制
 */
interface IAudioAgentService {
    /**
     * 1. 外部注入文本提问 (如桌面搜索框、第三方应用语音助手代调)
     * @param query 用户自然语言文本
     * @param extras 扩展上下文 (如当前所在应用包名、地理位置、屏幕上下文语义)
     */
    void submitUserQuery(String query, in Bundle extras);

    /**
     * 2. 外部硬打断控制 (如导航紧急警报、电话呼入、ADAS 安全告警触发微秒级打断)
     */
    void triggerBargeIn();

    /**
     * 3. 音频硬件录音流协同挂起 (第三方应用需要独占麦克风录音时发起友好协商)
     */
    void pauseAudioCapture();

    /**
     * 4. 音频硬件录音流恢复
     */
    boolean resumeAudioCapture();

    /**
     * 5. 注册多进程状态与事件监听回调
     */
    void registerCallback(IAudioAgentCallback callback);

    /**
     * 6. 注销多进程状态与事件监听回调
     */
    void unregisterCallback(IAudioAgentCallback callback);
}

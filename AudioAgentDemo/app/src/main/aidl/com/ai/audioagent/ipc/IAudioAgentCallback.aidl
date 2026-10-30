package com.ai.audioagent.ipc;

/**
 * 跨进程全双工 Agent 状态与事件异步监听回调接口 (IPC Callback)
 */
oneway interface IAudioAgentCallback {
    /**
     * 状态机流转通知
     * @param newState 当前状态 (IDLE=0, LISTENING=1, THINKING=2, SPEAKING=3)
     * @param oldState 上一状态
     */
    void onStateChanged(int newState, int oldState);

    /**
     * ASR 流式增量文字识别结果通知
     * @param partialText 识别出的文本片段或完整句子
     * @param isFinal 是否为尾点判决最终文本 (true 代表当前整句识别完成)
     */
    void onAsrStreaming(String partialText, boolean isFinal);

    /**
     * 流式 TTS 开始发声通知
     * @param sentence 正在朗读的文本分句
     */
    void onTtsSentenceStart(String sentence);

    /**
     * 异步工具执行结果通知 (包含高德导航/闹钟/车控等执行状态)
     * @param action 工具动作名称 (如 start_nav, set_alarm)
     * @param success 是否执行成功
     * @param resultSummary 执行概要信息或错误说明
     */
    void onToolExecuted(String action, boolean success, String resultSummary);
}

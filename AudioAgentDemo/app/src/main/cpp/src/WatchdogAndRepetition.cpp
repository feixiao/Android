#include "WatchdogAndRepetition.hpp"

namespace audio_agent {

TTFTWatchdog::TTFTWatchdog() = default;

TTFTWatchdog::~TTFTWatchdog() {
    Stop();
}

/**
 * @brief 启动首字延迟看门狗（TTFT Watchdog）
 * 
 * 在端侧全双工对话场景下，若首字发音延迟（TTFT）过长（如超过 800ms~1000ms），
 * 会导致用户误以为系统未听见而重复发问。
 * 本看门狗在启动思考时激活独立定时线程，若规定时间内未收到首句回调喂狗（Feed），
 * 则主动触发降级逻辑（如触发兜底提示音“请稍候，我正在思考”或切换云端引擎）。
 * 
 * @param timeout_ms 超时毫秒数（如 800ms）
 * @param on_timeout 超时未喂狗时的告警/降级回调函数
 */
void TTFTWatchdog::Start(int timeout_ms, std::function<void()> on_timeout) {
    Stop(); // 确保先前可能存在的定时线程已被妥善停止并回收

    timeout_ms_ = timeout_ms;
    on_timeout_ = std::move(on_timeout);
    fed_.store(false, std::memory_order_release);
    is_running_.store(true, std::memory_order_release);

    worker_thread_ = std::thread(&TTFTWatchdog::RunLoop, this);
}

/**
 * @brief 喂狗操作：当第一句有效文本（First Chunk）成功由断句器产出时调用
 * 
 * 瞬间解除告警，通知等待线程安全退出。
 */
void TTFTWatchdog::Feed() {
    fed_.store(true, std::memory_order_release);
    cv_.notify_all();
}

/**
 * @brief 停止看门狗并等待后台定时线程结束
 */
void TTFTWatchdog::Stop() {
    is_running_.store(false, std::memory_order_release);
    cv_.notify_all();

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

/**
 * @brief 看门狗核心等待循环（运行于独立线程）
 * 
 * 基于 std::condition_variable::wait_until 避免忙轮询，降低 CPU 负载。
 */
void TTFTWatchdog::RunLoop() {
    std::unique_lock<std::mutex> lock(mtx_);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);

    while (is_running_.load(std::memory_order_acquire)) {
        if (fed_.load(std::memory_order_acquire)) {
            // 首个数据包在超时前准时送达，看门狗成功解除
            return;
        }

        // 阻塞等待直到 deadline 或被 Feed()/Stop() 唤醒
        auto status = cv_.wait_until(lock, deadline);
        if (!is_running_.load(std::memory_order_acquire)) {
            return;
        }

        if (fed_.load(std::memory_order_acquire)) {
            return;
        }

        // 检查是否已经超时且未被喂狗
        if (status == std::cv_status::timeout || std::chrono::steady_clock::now() >= deadline) {
            is_running_.store(false, std::memory_order_release);
            lock.unlock(); // 触发回调前释放锁，防范外部回调可能产生的死锁
            if (on_timeout_) {
                on_timeout_();
            }
            return;
        }
    }
}

} // namespace audio_agent

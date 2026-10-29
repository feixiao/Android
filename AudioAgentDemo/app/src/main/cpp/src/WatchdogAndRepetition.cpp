#include "WatchdogAndRepetition.hpp"

namespace audio_agent {

TTFTWatchdog::TTFTWatchdog() = default;

TTFTWatchdog::~TTFTWatchdog() {
    Stop();
}

void TTFTWatchdog::Start(int timeout_ms, std::function<void()> on_timeout) {
    Stop(); // Ensure any previous timer thread is cleaned up

    timeout_ms_ = timeout_ms;
    on_timeout_ = std::move(on_timeout);
    fed_.store(false, std::memory_order_release);
    is_running_.store(true, std::memory_order_release);

    worker_thread_ = std::thread(&TTFTWatchdog::RunLoop, this);
}

void TTFTWatchdog::Feed() {
    fed_.store(true, std::memory_order_release);
    cv_.notify_all();
}

void TTFTWatchdog::Stop() {
    is_running_.store(false, std::memory_order_release);
    cv_.notify_all();

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void TTFTWatchdog::RunLoop() {
    std::unique_lock<std::mutex> lock(mtx_);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);

    while (is_running_.load(std::memory_order_acquire)) {
        if (fed_.load(std::memory_order_acquire)) {
            // First packet delivered in time! Watchdog disarmed.
            return;
        }

        auto status = cv_.wait_until(lock, deadline);
        if (!is_running_.load(std::memory_order_acquire)) {
            return;
        }

        if (fed_.load(std::memory_order_acquire)) {
            return;
        }

        if (status == std::cv_status::timeout || std::chrono::steady_clock::now() >= deadline) {
            // Timeout reached without being fed!
            is_running_.store(false, std::memory_order_release);
            lock.unlock();
            if (on_timeout_) {
                on_timeout_();
            }
            return;
        }
    }
}

} // namespace audio_agent

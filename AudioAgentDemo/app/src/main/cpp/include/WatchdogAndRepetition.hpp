#pragma once

#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <string>

namespace audio_agent {

/**
 * 3-Second First Token / First Packet Watchdog.
 * Protects against engine hangs, GPU shader stalls, or extreme noise loops.
 */
class TTFTWatchdog {
public:
    TTFTWatchdog();
    ~TTFTWatchdog();

    // Start timer with specified timeout in milliseconds (default 3000ms per whitepaper Section 6.3)
    void Start(int timeout_ms, std::function<void()> on_timeout);

    // Feed the watchdog when the first packet is successfully emitted
    void Feed();

    // Stop watchdog cleanly
    void Stop();

    // Default fallback speech prompt when TTFT times out or repetition loop triggers
    static const char* GetFallbackResponsePrompt() {
        return "抱歉，我刚走神了，您能再说一遍吗？";
    }

private:
    void RunLoop();

    std::atomic<bool> is_running_{false};
    std::atomic<bool> fed_{false};
    int timeout_ms_{3000};
    std::function<void()> on_timeout_;

    std::thread worker_thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
};

} // namespace audio_agent

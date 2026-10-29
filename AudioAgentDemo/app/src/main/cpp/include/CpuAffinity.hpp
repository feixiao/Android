#pragma once

#include <vector>
#include <cstddef>

namespace audio_agent {

/**
 * CPU Core Information
 */
struct CpuCoreInfo {
    int id{0};
    long max_freq_khz{0};
};

/**
 * Detects performance cores by reading sysfs on Linux / Android.
 * Returns the top_n CPU core IDs with the highest maximum frequency.
 */
std::vector<int> DetectTopPerformanceCores(size_t top_n = 4);

/**
 * Detects efficiency (little) cores by reading sysfs on Linux / Android.
 * Returns the bottom_n CPU core IDs with the lowest maximum frequency.
 */
std::vector<int> DetectEfficiencyCores(size_t bottom_n = 2);

/**
 * Binds the calling thread to the specified CPU cores.
 * On Android, uses sched_setaffinity(gettid(), ...).
 * Returns true if affinity was successfully set.
 */
bool BindCurrentThreadToCores(const std::vector<int>& core_ids);

/**
 * Convenience method: Binds calling thread to top performance cores.
 */
bool BindThreadToBigCores(size_t top_n = 4);

/**
 * Convenience method: Binds calling thread to efficiency cores (VAD / ASR).
 */
bool BindThreadToLittleCores(size_t bottom_n = 2);

} // namespace audio_agent

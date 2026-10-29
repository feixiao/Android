#include "CpuAffinity.hpp"

#include <fstream>
#include <algorithm>
#include <string>
#include <unistd.h>

#if defined(__linux__) || defined(__ANDROID__)
#include <sched.h>
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <mach/mach_types.h>
#include <mach/thread_act.h>
#endif

namespace audio_agent {

std::vector<int> DetectTopPerformanceCores(size_t top_n) {
    std::vector<CpuCoreInfo> cores;

    long num_cpus = sysconf(_SC_NPROCESSORS_CONF);
    if (num_cpus <= 0) {
        num_cpus = 4; // safe fallback
    }

    for (int i = 0; i < num_cpus; ++i) {
        std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/cpuinfo_max_freq";
        std::ifstream ifs(path);
        long freq = 0;
        if (ifs >> freq) {
            cores.push_back({i, freq});
        } else {
            // Sysfs might not expose cpufreq on emulators or virtual environments; fallback based on index
            cores.push_back({i, 1000000 + i * 100000});
        }
    }

    // Sort by maximum frequency descending (Super-Big -> Big -> Little)
    std::sort(cores.begin(), cores.end(), [](const CpuCoreInfo& a, const CpuCoreInfo& b) {
        return a.max_freq_khz > b.max_freq_khz;
    });

    std::vector<int> result;
    size_t count = std::min(top_n, cores.size());
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        result.push_back(cores[i].id);
    }
    return result;
}

std::vector<int> DetectEfficiencyCores(size_t bottom_n) {
    std::vector<CpuCoreInfo> cores;
    long num_cpus = sysconf(_SC_NPROCESSORS_CONF);
    if (num_cpus <= 0) num_cpus = 4;

    for (int i = 0; i < num_cpus; ++i) {
        std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/cpuinfo_max_freq";
        std::ifstream ifs(path);
        long freq = 0;
        if (ifs >> freq) {
            cores.push_back({i, freq});
        } else {
            cores.push_back({i, 1000000 + i * 100000});
        }
    }

    // Sort by frequency ascending (Little cores first)
    std::sort(cores.begin(), cores.end(), [](const CpuCoreInfo& a, const CpuCoreInfo& b) {
        return a.max_freq_khz < b.max_freq_khz;
    });

    std::vector<int> result;
    size_t count = std::min(bottom_n, cores.size());
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        result.push_back(cores[i].id);
    }
    return result;
}

bool BindCurrentThreadToCores(const std::vector<int>& core_ids) {
    if (core_ids.empty()) return false;

#if defined(__linux__) || defined(__ANDROID__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int id : core_ids) {
        CPU_SET(id, &cpuset);
    }
    pid_t tid = gettid();
    int rc = sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset);
    return rc == 0;
#elif defined(__APPLE__)
    // macOS does not provide sched_setaffinity; mach thread affinity tags are advisory hints
    thread_affinity_policy_data_t policy = { core_ids.front() };
    mach_port_t mach_thread = pthread_mach_thread_np(pthread_self());
    kern_return_t kr = thread_policy_set(
        mach_thread,
        THREAD_AFFINITY_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_AFFINITY_POLICY_COUNT
    );
    return kr == KERN_SUCCESS;
#else
    return false;
#endif
}

bool BindThreadToBigCores(size_t top_n) {
    auto big_cores = DetectTopPerformanceCores(top_n);
    return BindCurrentThreadToCores(big_cores);
}

bool BindThreadToLittleCores(size_t bottom_n) {
    auto little_cores = DetectEfficiencyCores(bottom_n);
    return BindCurrentThreadToCores(little_cores);
}

} // namespace audio_agent

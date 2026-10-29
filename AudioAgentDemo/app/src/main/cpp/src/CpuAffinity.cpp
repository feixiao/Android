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

/**
 * @brief 检测并返回当前设备中最高主频的性能大核心（Super-Big / Big Cores）物理编号列表
 * 
 * 原理：
 * 1. 通过 sysconf(_SC_NPROCESSORS_CONF) 获取当前系统逻辑 CPU 总核数。
 * 2. 依次遍历 Linux/Android 虚拟文件系统:
 *    /sys/devices/system/cpu/cpu[0..N-1]/cpufreq/cpuinfo_max_freq
 *    读取各核心硬件最高主频（单位 kHz）。
 * 3. 若处于模拟器或虚拟环境中导致 sysfs 节点缺失，采用索引升序的保底主频估算。
 * 4. 按 max_freq_khz 降序排列，截取前 top_n 个大核，用于绑定 LLM 推理等重度算力线程。
 */
std::vector<int> DetectTopPerformanceCores(size_t top_n) {
    std::vector<CpuCoreInfo> cores;

    long num_cpus = sysconf(_SC_NPROCESSORS_CONF);
    if (num_cpus <= 0) {
        num_cpus = 4; // 保底默认值
    }

    for (int i = 0; i < num_cpus; ++i) {
        std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/cpuinfo_max_freq";
        std::ifstream ifs(path);
        long freq = 0;
        if (ifs >> freq) {
            cores.push_back({i, freq});
        } else {
            // 在模拟器或沙箱环境下无 cpufreq 节点，按核心编号模拟差异化主频
            cores.push_back({i, 1000000 + i * 100000});
        }
    }

    // 按最大主频从高到低排序：超大核 (Cortex-X系列) -> 大核 (Cortex-A7系列) -> 小核 (Cortex-A5系列)
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

/**
 * @brief 检测并返回当前设备中主频最低的能效小核心（Little Cores）物理编号列表
 * 
 * 适用于低负载、常驻轮询后台任务（例如闲置记忆归纳线程），防止占用大核引起发热或挤占前台算力。
 */
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

    // 按最大主频从低到高排序：小核排在最前面
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

/**
 * @brief 将当前调用线程强制绑定到指定的 CPU 核心集合上
 * 
 * @param core_ids 允许执行该线程的 CPU 物理编号列表
 * @return true 绑定成功；false 绑定失败
 */
bool BindCurrentThreadToCores(const std::vector<int>& core_ids) {
    if (core_ids.empty()) return false;

#if defined(__linux__) || defined(__ANDROID__)
    // Linux/Android 原生通过 sched_setaffinity 系统调用设置线程 CPU 亲和性掩码
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int id : core_ids) {
        CPU_SET(id, &cpuset);
    }
    pid_t tid = gettid();
    int rc = sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset);
    return rc == 0;
#elif defined(__APPLE__)
    // macOS/Darwin 不支持 POSIX sched_setaffinity，通过 Mach 线程亲和性策略提供调度建议（Advisory hint）
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

/**
 * @brief 绑定当前线程至排名前 top_n 的高性能核心
 */
bool BindThreadToBigCores(size_t top_n) {
    auto big_cores = DetectTopPerformanceCores(top_n);
    return BindCurrentThreadToCores(big_cores);
}

/**
 * @brief 绑定当前线程至排名靠后的能效小核心
 */
bool BindThreadToLittleCores(size_t bottom_n) {
    auto little_cores = DetectEfficiencyCores(bottom_n);
    return BindCurrentThreadToCores(little_cores);
}

} // namespace audio_agent

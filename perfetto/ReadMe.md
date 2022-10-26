## perfetto快速入门

+ [《Perfetto入门详解》](https://www.jianshu.com/p/f4cf101cc64f)
+ 
#### 调查Android内存占用问题
```shell
adb shell perfetto \
  -c - --txt \
  -o /data/misc/perfetto-traces/trace \
<<EOF

buffers: {
    size_kb: 63488
    fill_policy: DISCARD
}
buffers: {
    size_kb: 2048
    fill_policy: DISCARD
}
data_sources: {
    config {
        name: "linux.process_stats"
        target_buffer: 1
        process_stats_config {
            scan_all_processes_on_start: true
            proc_stats_poll_ms: 1000
        }
    }
}
data_sources: {
    config {
        name: "android.log"
        android_log_config {
        }
    }
}
data_sources: {
    config {
        name: "linux.sys_stats"
        sys_stats_config {
            meminfo_period_ms: 1000
            vmstat_period_ms: 1000
        }
    }
}
data_sources: {
    config {
        name: "android.heapprofd"
        target_buffer: 0
        heapprofd_config {
            sampling_interval_bytes: 4096
            shmem_size_bytes: 8388608
            block_client: true
        }
    }
}
data_sources: {
    config {
        name: "android.java_hprof"
        target_buffer: 0
        java_hprof_config {
            continuous_dump_config {
                dump_interval_ms: 60000
            }
        }
    }
}
data_sources: {
    config {
        name: "linux.ftrace"
        ftrace_config {
            ftrace_events: "mm_event/mm_event_record"
            ftrace_events: "kmem/rss_stat"
            ftrace_events: "ion/ion_stat"
            ftrace_events: "dmabuf_heap/dma_heap_stat"
            ftrace_events: "kmem/ion_heap_grow"
            ftrace_events: "kmem/ion_heap_shrink"
            ftrace_events: "sched/sched_process_exit"
            ftrace_events: "sched/sched_process_free"
            ftrace_events: "task/task_newtask"
            ftrace_events: "task/task_rename"
            ftrace_events: "lowmemorykiller/lowmemory_kill"
            ftrace_events: "oom/oom_score_adj_update"
            ftrace_events: "ftrace/print"
            atrace_apps: "lmkd"
        }
    }
}
duration_ms: 300000

EOF


# 30 秒后获取内容
adb pull /data/misc/perfetto-traces/trace ./mem-trace
```


#### 参考资料
+ [《Perfetto入门详解》](https://www.jianshu.com/p/f4cf101cc64f)
+ [《Heap profiling》](https://perfetto.dev/docs/quickstart/heap-profiling)
+ [《Debugging memory usage on Android》](https://perfetto.dev/docs/case-studies/memory)
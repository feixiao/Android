## perfetto快速入门

#### 调查Android内存占用问题
```shell
adb shell perfetto \
  -c - --txt \
  -o /data/misc/perfetto-traces/trace \
<<EOF

buffers: {
    size_kb: 8960
    fill_policy: DISCARD
}
buffers: {
    size_kb: 1280
    fill_policy: DISCARD
}
data_sources: {
    config {
        name: "linux.process_stats"
        target_buffer: 1
        process_stats_config {
            scan_all_processes_on_start: true
        }
    }
}
data_sources: {
    config {
        name: "linux.ftrace"
        ftrace_config {
            ftrace_events: "mm_event/mm_event_record"
            ftrace_events: "kmem/rss_stat"
            ftrace_events: "kmem/ion_heap_grow"
            ftrace_events: "kmem/ion_heap_shrink"
        }
    }
}
duration_ms: 30000

EOF


# 30 秒后获取内容
adb pull /data/misc/perfetto-traces/trace ./mem-trace
```


#### 参考资料
+ [《Heap profiling》](https://perfetto.dev/docs/quickstart/heap-profiling)
+ [《Debugging memory usage on Android》](https://perfetto.dev/docs/case-studies/memory)
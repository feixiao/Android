# AI Audio Agent 角色、规则与约束规范 (Agent.md)

本文档定义了 **Android 端侧全双工 AI Audio Agent** 的系统角色定位、核心交互规范、底层架构工程准则及行为约束。

---

## 一、 角色定义 (Role & Persona)

### 1.1 核心身份
* **名称**：端侧全双工智能语音助理 (On-Device Full-Duplex Audio Agent)。
* **定位**：运行于 Android 端侧（手机、车机、智能座舱或 IoT 硬件）的高性能实时语音助手。
* **核心性格**：专业、温暖、高效、拟人化，富有同理心且反应敏捷。

### 1.2 表达风格
* **极简口语化**：坚决避免“书面语”、“公文腔”与冗长排比，所有回答必须符合人类日常口语交流习惯。
* **首句极短优先 (TTFT 压榨)**：首句控制在 **2~7 个字**（6~21 字节，如“好的，”、“没问题，”、“已为您搞定，”），遇标点立刻切片推入 TTS，将首音延迟压缩至 **150ms 左右**。
* **纯文字发音友好**：
  * **严禁输出 Markdown 标记**（如 `**加粗**`、`# 标题`、`- 列表`、````代码块```` 等）。
  * **数字汉字化**：所有阿拉伯数字一律转为汉字发音（如“二十四度”而非“24度”；“十一点三十分”而非“11:30”）。
  * **严禁朗读特殊标点与符号**（如 `@`、`#`、`$`、`_` 等）。

---

## 二、 交互原则与行为约束 (Interaction Constraints)

### 2.1 绝不答非所问 (Relevance Invariant)
* **严禁伪造用户问题**：系统绝不可自作聪明或死板循环替用户塞入未曾说过的问题。
* **真实意图对齐**：必须 100% 针对用户实际说出或输入的 Query 进行语义理解、解答或工具调用。
* **超范围问题处理**：若遇到开放提问或知识盲区，诚实说明并给出实质性建议或引导，杜绝机械式套用功能介绍模板。

### 2.2 工具调用规范 (Tool Calling Protocol)
* **“先安抚，后执行”**：
  * 在执行耗时或系统控制指令时，必须在第一句给出即时口语安抚。
  * 紧随其后输出结构化标签：`<|tool_call|>{"action":"<ACTION_NAME>","params":...}<|/tool_call|>`。
* **语音与控制分流**：
  * 流式分词切句器（`StreamingSentenceChunker`）在检测到 `<|tool_call|>` 标签时，**严禁将 JSON 内容送入 TTS 队列朗读**。
  * JSON 必须被无感拦截并分流至异步执行器（`SystemToolExecutor`），实现系统静默控制。

---

## 三、 系统底层工程规则 (Architectural Engineering Rules)

### 3.1 状态机流转 (FSM Transitions)
系统严格遵循四态有限状态机规范，禁止非法跃迁：
```text
  ┌────────────────────────────────────────────────────────┐
  │                                                        │
  ▼                                                        │
[IDLE] ──(人声检测 avg_energy > 门限)──► [LISTENING]       │
  ▲                                           │            │
  │                                     (VAD 尾点落音)     │
  │                                           ▼            │
  │                                      [THINKING]        │
  │                                           │            │
  │                                    (首句 Token 吐出)   │
  │                                           ▼            │
  └───────────────(播报自然完毕)───────── [SPEAKING]        │
                                              │            │
                                       (用户说话插话)      │
                                              ▼            │
                                      [Barge-in 硬件打断] ──┘
```

### 3.2 毫秒级打断铁律 (Barge-in Interrupt)
当处于 `THINKING`（思考）或 `SPEAKING`（播报）状态中，一旦麦克风采集到用户新的人声：
1. **原子标志位置位**：`abort_flag_.store(true)`，微秒级通知 LLM 解码循环与 TTS 合成循环立即退出。
2. **喇叭瞬间静音**：立即排空扬声器 SPSC 环形缓冲区（`spk_ring_buffer_->clear()`），扬声器在 **5ms 内彻底静音**。
3. **队列清空**：瞬时清空待合成与待播报的文本队列。
4. **记忆回滚与 KV Cache 对齐**：
   * 精确统计用户耳朵实际听到的短句累计量。
   * 以 `“已播报内容（被用户打断）”` 覆盖记忆，回滚 LLM 未播报部分的 KV Cache，确保人机记忆绝对对齐。
5. **平滑接入监听**：状态机无缝跃迁至 `LISTENING`，准备接收用户的打断新指令。

### 3.3 音频底座与线程安全规范 (Audio Foundation)
1. **硬件 VoiceCommunication 配对**：
   * Oboe 录音流配置 `InputPreset::VoiceCommunication`，强制激活芯片硬件 DSP AEC/NS/AGC。
   * 播放流配置 `Usage::VoiceCommunication`，与录音流形成参考回路，消除扬声器自激反馈。
2. **音频实时线程禁令**：
   * 在 Oboe `DataCallback` 实时音频回调中，**严禁使用 Mutex 互斥锁、系统调用或动态堆内存分配（Zero-malloc）**。
   * 跨线程音频流转必须通过 64B 缓存行对齐的无锁单生产者单消费者缓冲区（`LockFreeSPSCRingBuffer`）。

### 3.4 异构算力与功耗调度 (CPU Scheduling)
* **感知线程 (Perception Thread)**：绑定至能效小核（Little Cores），将待机能耗限制在 150mW 以内。
* **认知与表达线程 (Cognition & Expression Threads)**：动态绑定至高频性能大核或超大核（Big/Cortex-X Cores），保障首字低延迟。

---

## 四、 容错与防御性准则 (Watchdog & Fault Tolerance)

1. **3秒首包看门狗 (TTFT Watchdog)**：
   * 从接收到 User Query 进入 `THINKING` 起计时，若 3000ms 内未能输出首个可播报标点短句，立即中断并触发安抚语：“抱歉，我刚走神了，您能再说一遍吗？”
2. **N-gram 重复死循环拦截**：
   * 流式切句器内置 12 字节滑动窗口，实时监测小模型常见的生成复读机死循环；一旦检测到重复，立即截断并播报兜底话术。
3. **温控降频防御 (Thermal Defense)**：
   * 监听 Android `PowerManager.OnThermalStatusChangedListener`；在设备达到 `MODERATE` 及以上温控预警时，动态降低并发推理线程或缩减最大生成 Token 数，防止设备过热降频导致音频爆音破音（Underrun Glitch）。

---

## 五、 模块接口规范 (Engine Interfaces)

* **感知引擎 (`ISpeechPerceptionEngine`)**：负责 PCM 音频送入、VAD 语音起止检测及流式 ASR 文本产出。
* **认知引擎 (`ILlmEngine`)**：接收多轮对话上下文 Prompt，向流式输出流（`std::ostream`）逐 Token 生成回答。
* **表达引擎 (`ISpeechSynthesisEngine`)**：接收流式切句文本，分块合成 PCM 音频，支持实时 Barge-in 终止回调。

---

## 六、 ISO C++17 编码风格与质量控制规范 (C++ Style & Quality Control)

为确保端侧实时音频全双工引擎的低延迟、零内存泄漏与工业级稳定性，所有 Native C++ 代码必须严格遵守以下规范：

### 6.1 语言标准与编译控制
* **基准标准**：严格基于 **ISO C++17** (`set(CMAKE_CXX_STANDARD 17)`)，禁止使用已废弃特性（如 `std::auto_ptr`、`register` 等）。
* **警告与优化级别**：
  * 编译必须启用 `-O3 -fPIC -Wall -Wextra`，严禁关闭有意义的警告。
  * ARM64 专用优化：在 `arm64-v8a` 架构下开启 `-march=armv8.2-a+dotprod+fp16` 向量化算子加速。

### 6.2 实时音频线程“三无法则” (Hard Real-Time Audio Constraints)
在 Oboe 音频回调函数（`onAudioReady`）及与之紧密绑定的实时处理函数中，必须遵循：
1. **零堆内存分配 (Zero-malloc)**：严禁在实时线程内调用 `malloc`、`free`、`new`、`delete`，严禁触发 `std::vector::push_back` 扩容或动态 `std::string` 拼接。所有中间缓冲必须在初始化时完成预分配。
2. **零互斥锁阻塞 (Zero-mutex)**：严禁使用 `std::mutex`、`std::condition_variable`、信号量等会引起线程睡眠和上下文切换的系统同步原语。所有数据交换必须使用无锁单生产者单消费者缓冲区（`LockFreeSPSCRingBuffer`）。
3. **零系统调用与 I/O (Zero-syscall)**：严禁在音频回调中调用磁盘写操作、`printf`、`std::cout` 或 `__android_log_print`（日志写入可能引发内核级文件锁阻塞导致音频 Underrun 爆音）。

### 6.3 命名与风格规范 (Google C++ 规范改良版)
* **类型与类名**：`PascalCase`（如 `AudioAgentPipeline`, `StreamingSentenceChunker`）。
* **函数与方法名**：`PascalCase`（如 `Start()`, `TriggerBargeIn()`, `AcceptAudioChunk()`）。
* **局部变量与函数入参**：`snake_case`（如 `sample_rate`, `buffer_size`）。
* **类私有成员变量**：`snake_case_`（必须带有末尾下划线，如 `mic_ring_buffer_`, `abort_flag_`）。
* **常量与宏**：
  * 编译期常量使用 `constexpr` + `UPPER_SNAKE_CASE`（如 `DEFAULT_MIC_SAMPLE_RATE`）。
  * 宏定义一律使用全大写前缀（如 `AUDIO_AGENT_USE_OBOE`）。
* **枚举类型**：统一采用带作用域的枚举类 `enum class`，枚举项使用全大写（如 `AgentState::LISTENING`）。

### 6.4 资源管理与生命周期 (RAII & JNI 边界)
1. **独占与共享语义**：
   * 优先使用 `std::unique_ptr` 表达明确所有权；仅在跨线程共享数据底座（如 RingBuffer、引擎句柄）时使用 `std::shared_ptr`。
   * 严禁裸指针（Raw Pointer）持有资源所有权，禁止直接显式调用 `delete`。
2. **JNI 边界内存与局部引用清理**：
   * JNI 线程回调前必须正确附加（`AttachCurrentThread`）并在退出时解绑（`DetachCurrentThread`）。
   * 跨线程持有 Java 对象必须提升为 `NewGlobalRef`，并在实例析构时成对调用 `DeleteGlobalRef`。
   * 高频 JNI 回调（如增量 ASR 输出）创建的本地引用（`jstring`），用毕必须立即调用 `DeleteLocalRef` 释放，防止 JNI Local Reference 表溢出（512 限制）。

### 6.5 内存序与伪共享规避 (Memory Order & Cacheline Alignment)
1. **精确原子内存序**：
   * 严禁不加思考全量使用重度开销的 `std::memory_order_seq_cst`。
   * 跨线程中断信号与数据传递采用标准 **Acquire-Release 语义**（写入用 `memory_order_release`，读取用 `memory_order_acquire`）。
   * 仅用于统计或无因果同步依赖的计数器采用 `memory_order_relaxed`。
2. **消除伪共享 (False Sharing)**：
   * 环形缓冲区的读指针 `head_` 与写指针 `tail_` 必须使用 `alignas(64)` 显式对齐到 64 字节缓存行，杜绝多核多线程下的 Cacheline 频繁失效颠簸。

### 6.6 自动化测试与质量底线 (Quality Assurance)
1. **宿主机冒烟自测 (`make test`)**：
   * 所有 Native C++ 核心子系统（无锁环形缓冲区、流式切句器、看门狗、三级记忆、异构大小核感知）必须完全脱离 Android NDK，在宿主机（macOS / Linux）上能够通过 `clang++ -std=c++17` 零依赖编译并运行自动化单元测试。
   * 提交代码前必须保证宿主机测试套件通过率 **100%**。
2. **内存泄漏与未定义行为检测**：
   * 关键无锁算法在开发阶段必须开启 AddressSanitizer (`-fsanitize=address,undefined`) 进行死锁、越界与并发竞争校验。

# Android 端侧 Demo 开源来源

本目录的目标检测应用基于公开的 Android CameraX + LiteRT/MediaPipe Tasks 模式整理。硬件后端、帧率、延迟、温度和功耗必须在目标设备上重新测量；文档不预设 Pixel 10 的 SoC 内部名称或固定 benchmark。

## 可直接复用的官方/上游代码

| 场景 | 上游仓库 | 关键目录或入口 | 本地工程对应部分 |
| --- | --- | --- | --- |
| CameraX 实时目标检测 | [google-ai-edge/mediapipe-samples](https://github.com/google-ai-edge/mediapipe-samples) | [`examples/object_detection/android`](https://github.com/google-ai-edge/mediapipe-samples/tree/main/examples/object_detection/android) | `ObjectDetectorDemo` 的 CameraX、权限、Overlay 和模型推理管线 |
| Android 离线文本生成 | [ggerganov/llama.cpp](https://github.com/ggerganov/llama.cpp) | [`examples/distribution/java`](https://github.com/ggml-org/llama.cpp/tree/master/examples/distribution/java)、Android 构建说明以仓库当前版本为准 | 尚未复制模型或原生库；按上游构建后接入独立模块 |
| Android GPU/Vulkan LLM | [mlc-ai/mlc-llm](https://github.com/mlc-ai/mlc-llm) | [`android`](https://github.com/mlc-ai/mlc-llm/tree/main/android) | 尚未复制模型或预编译 runtime；按上游 release 与设备支持矩阵验证 |

上游项目的版本、构建入口、模型清单和许可证可能变化。使用时固定 commit/tag，并阅读对应仓库的许可证与模型许可证。
## Pixel 10 (Tensor G5) 端侧 AI 能力演进与突破

结合上述三个开源技术栈（CameraX + LiteRT 实时视觉流、llama.cpp 端侧离线大模型、MLC-LLM GPU/Vulkan 加速推理），Pixel 10（搭载基于台积电 3nm 工艺与全新架构的 Google Tensor G5 芯片）在端侧 AI 算力与能效上相比前代（Pixel 8/9，Tensor G3/G4）实现了质的跨越，解锁了之前受限于算力、带宽和温控而无法落地的能力：

### 1. 实时全帧率视觉流持续感知（对应 LiteRT + CameraX）
* **前代痛点**：在连续 60fps 取流或高分辨率相机流上运行密集型检测/分割模型时，发热严重、易快速触发系统温控降频（Thermal Throttling），导致帧率急剧跌落至 15~24fps 甚至引发 UI 掉帧，开发者往往被迫降采样（仅抽取关键帧或跑极简小模型）。
* **Pixel 10 突破**：
  * **60fps 持续零拷贝硬件直通**：Tensor G5 的全新 Edge TPU 算力提升约 60%，单帧纯推理延迟压缩到个位数毫秒级（<5ms），高负载连续取流不掉帧。
  * **取景器实时多模态交互（Camera Coach）**：AI 视觉理解从“拍完照后的静态后处理”演进为“取景器预览期间的毫秒级实时流式感知”，能在取景阶段实时分析光影、构图与被摄主体姿态并提供动态运镜指引。

### 2. 端侧完全离线 7B+ 大语言模型与多模态 VLM（对应 llama.cpp / MLC-LLM）
* **前代痛点**：前代机型受限于统一内存带宽与 NPU/GPU 吞吐，端侧仅能勉强运行 1B~2B 参数的超轻量模型，且主要限制在特定预设功能（如录音机摘要）；离线运行 7B~8B 量化模型（如 Llama-3-8B、Gemma-7B）时，首 token 延迟（TTFT）极长，且生成速度通常低至 2~4 tokens/s，伴随剧烈发热与内存压力，无法端侧实时处理多模态图像问答。
* **Pixel 10 突破**：
  * **离线 7B~8B 模型可用吞吐生成**：通过 `llama.cpp` 原生 JNI 绑定或 `mlc-llm` 的 Vulkan 计算管线，可离线以 10+ tokens/s 的实用吞吐运行 7B~8B 级别量化模型，实现长文本离线总结与本地代码解析。
  * **端侧多模态图文问答 (VLM)**：端侧具备足够算力直接将 CameraX 捕获帧送入端侧多模态模型进行本地实时图文问答，彻底脱离对云端 API 的依赖与隐私上报风险。

### 3. 视频画质增强 (Video Boost) 由“纯云端回传”转向“端侧本地加速”
* **前代痛点**：如 Pixel 8/9 的 Video Boost（视频夜视与防抖增强）必须将数十 GB 的原始 Raw 视频文件完整上传至 Google 云端数据中心集群进行渲染排队，耗费大量流量且处理等待时间长达数十分钟至数小时。
* **Pixel 10 突破**：
  * **端侧硬件级协同渲染**：Tensor G5 将升级版 TPU 算力直接与 ISP 硬件深度耦合，原先必须由云端数据中心执行的复杂降噪、动态范围重建与运动时域对齐，大幅度迁移至手机端侧即时或近实时硬件加速完成。

### 4. 前后代能力对比汇总

| 场景 | 之前（Pixel 8 / Pixel 9 时代） | Pixel 10（Tensor G5 时代） |
| --- | --- | --- |
| **相机实时视觉流** | 长时间运行易过热降频掉帧，复杂模型难以常驻 60fps | 60fps 持续零拷贝推理，延迟稳定 <5ms，支持取景框实时多模态指导 |
| **端侧大语言模型 (LLM)** | 局限于 2B 微型模型；跑 7B 模型吞吐过低（<3 tokens/s）且严重发热 | 离线顺畅运行 7B~8B 量化模型（llama.cpp / MLC-LLM），具备实用吞吐与低首字延迟 |
| **视频画质增强 (Video Boost)** | **完全依赖云端**：手机上传原片到云端排队数十分钟渲染 | **端侧即时/近实时处理**：TPU + ISP 本地高能效加速，显著降低对云端的依赖 |
| **系统级主动感知** | 被动响应式（需手动截屏或呼出语音助手） | **Magic Cue 等系统级常驻**：后台低功耗理解多应用上下文与屏幕意图 |

### 5. 工程开发落地规范
* **执行后端选型**：Android 系统正逐步将 NPU 驱动标准从传统 NNAPI 演进为 LiteRT 原生加速委托（LiteRT TPU / OpenCL Delegate）。在 Pixel 10 上开发时，推荐优先使用最新 Google AI Edge 原生驱动接口，以避免系统抽象层额外的 JNI 与调度开销。GPU 计算架构深度解析（Vulkan Compute vs. OpenCL、AHardwareBuffer 零拷贝直通等）请参阅专栏文档 [`VULKAN_COMPUTE.md`](VULKAN_COMPUTE.md)。
* **内存管道零拷贝**：高吞吐视觉模型必须严格遵守 `STRATEGY_KEEP_ONLY_LATEST` 并按 `rowStride` 复用缓冲区，禁止在每帧分析回调中重复分配 Bitmap 或数组，避免引发 GC 停顿破坏 60fps 硬件加速流水线。

## 构建目标检测 Demo

```bash
cd ObjectDetectorDemo
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

应用使用 `ImageAnalysis.OUTPUT_IMAGE_FORMAT_RGBA_8888` 和 `STRATEGY_KEEP_ONLY_LATEST`。代码按 `rowStride` 逐行打包 RGBA 数据，再复用 Bitmap 送入检测器；不得把 `ImageProxy` 的 plane 当成无 padding 的紧凑数组。

当前 Demo 提供 CPU、GPU 和 NNAPI 选项，但“可选”不等于“设备一定支持”：初始化失败时以 logcat 和实际回调为准。NNAPI 不代表某个固定 NPU/TPU，系统会依据设备驱动决定可用执行路径。

## 获取上游代码

不把三个大型仓库复制进本仓库。需要完整官方样例时，使用浅克隆并固定提交：

```bash
git clone --depth 1 https://github.com/google-ai-edge/mediapipe-samples.git
# llama.cpp 的 Android/Java 入口以仓库当前目录为准
git clone --depth 1 https://github.com/ggml-org/llama.cpp.git
git clone --depth 1 https://github.com/mlc-ai/mlc-llm.git
```

这三个项目应作为独立工程或独立 Gradle/NDK 模块评估，不建议把模型文件、原生 runtime 和目标检测 APK 混成一个演示包。

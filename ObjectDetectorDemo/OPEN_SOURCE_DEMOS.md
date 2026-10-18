# Android 端侧 Demo 开源来源

本目录的目标检测应用基于公开的 Android CameraX + LiteRT/MediaPipe Tasks 模式整理。硬件后端、帧率、延迟、温度和功耗必须在目标设备上重新测量；文档不预设 Pixel 10 的 SoC 内部名称或固定 benchmark。

## 可直接复用的官方/上游代码

| 场景 | 上游仓库 | 关键目录或入口 | 本地工程对应部分 |
| --- | --- | --- | --- |
| CameraX 实时目标检测 | [google-ai-edge/mediapipe-samples](https://github.com/google-ai-edge/mediapipe-samples) | [`examples/object_detection/android`](https://github.com/google-ai-edge/mediapipe-samples/tree/main/examples/object_detection/android) | `ObjectDetectorDemo` 的 CameraX、权限、Overlay 和模型推理管线 |
| Android 离线文本生成 | [ggerganov/llama.cpp](https://github.com/ggerganov/llama.cpp) | [`examples/distribution/java`](https://github.com/ggml-org/llama.cpp/tree/master/examples/distribution/java)、Android 构建说明以仓库当前版本为准 | 尚未复制模型或原生库；按上游构建后接入独立模块 |
| Android GPU/Vulkan LLM | [mlc-ai/mlc-llm](https://github.com/mlc-ai/mlc-llm) | [`android`](https://github.com/mlc-ai/mlc-llm/tree/main/android) | 尚未复制模型或预编译 runtime；按上游 release 与设备支持矩阵验证 |

上游项目的版本、构建入口、模型清单和许可证可能变化。使用时固定 commit/tag，并阅读对应仓库的许可证与模型许可证。

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

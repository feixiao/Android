package com.google.aiedge.objectdetectordemo.helper

import android.content.Context
import android.graphics.Bitmap
import android.os.SystemClock
import android.util.Log
import org.tensorflow.lite.gpu.CompatibilityList
import org.tensorflow.lite.support.image.ImageProcessingOptions
import org.tensorflow.lite.support.image.TensorImage
import org.tensorflow.lite.task.core.BaseOptions
import org.tensorflow.lite.task.vision.detector.Detection
import org.tensorflow.lite.task.vision.detector.ObjectDetector

/**
 * 2026 Google AI Edge (LiteRT) 官方标准目标检测辅助类
 *
 * 核心特性：
 * 1. 深度适配 Google Pixel 10 (Tensor G5 芯片)，支持通过 NNAPI / LiteRT 驱动直通 Google Edge TPU 硬件加速器；
 * 2. 具备异构硬件动态下拉热切换能力：
 *    - DELEGATE_CPU: 支持 1~8 线程动态调节与 CPU 核心负载调优；
 *    - DELEGATE_GPU: 支持 OpenCL / OpenGL 移动端 GPU 硬件着色器加速；
 *    - DELEGATE_NNAPI: 激活 Tensor G5 Edge TPU 矩阵协处理器（毫秒级 INT8 高并发推理）；
 * 3. 实时统计并回调纯硬件端侧推理延迟 (ms)。
 */
class ObjectDetectorHelper(
    var currentThreshold: Float = 0.5f,
    var numThreads: Int = 4,
    var maxResults: Int = 5,
    var currentDelegate: Int = DELEGATE_NNAPI,
    var currentModel: Int = MODEL_EFFICIENTDETV0,
    val context: Context,
    val objectDetectorListener: DetectorListener?
) {

    private var objectDetector: ObjectDetector? = null

    init {
        setupObjectDetector()
    }

    /**
     * 清理并释放底层 LiteRT 原生 C++ 运行句柄
     */
    fun clearObjectDetector() {
        objectDetector?.close()
        objectDetector = null
    }

    /**
     * 重新构建与初始化 LiteRT ObjectDetector 引擎
     */
    fun setupObjectDetector() {
        // 1. 配置硬件加速与线程 BaseOptions
        val baseOptionsBuilder = BaseOptions.builder()
            .setNumThreads(numThreads)

        when (currentDelegate) {
            DELEGATE_CPU -> {
                // 纯 CPU 模式：依赖 ARMv9 核心集群并受 numThreads 控制
                Log.i(TAG, "LiteRT 初始化硬件加速模式: CPU (Threads: $numThreads)")
            }
            DELEGATE_GPU -> {
                // 移动 GPU 模式 (Immortalis)
                if (CompatibilityList().isDelegateSupportedOnThisDevice) {
                    baseOptionsBuilder.useGpu()
                    Log.i(TAG, "LiteRT 初始化硬件加速模式: GPU (Vulkan/OpenCL Delegate)")
                } else {
                    objectDetectorListener?.onError("当前设备不支持 GPU Delegate 硬件加速，已降级至 CPU")
                }
            }
            DELEGATE_NNAPI -> {
                // Pixel 10 Tensor G5 Edge TPU 直通模式 (通过 Android NNAPI)
                baseOptionsBuilder.useNnapi()
                Log.i(TAG, "LiteRT 初始化硬件加速模式: NNAPI / Edge TPU (Google Tensor G5)")
            }
        }

        // 2. 配置目标检测参数 (阈值与最大候选框)
        val optionsBuilder = ObjectDetector.ObjectDetectorOptions.builder()
            .setBaseOptions(baseOptionsBuilder.build())
            .setScoreThreshold(currentThreshold)
            .setMaxResults(maxResults)

        // 3. 选择对应模型文件 (内置于 assets/models/)
        val modelName = when (currentModel) {
            MODEL_EFFICIENTDETV0 -> "models/efficientdet_lite0.tflite"
            MODEL_MOBILENETV1 -> "models/mobilenetv1_quant.tflite"
            else -> "models/efficientdet_lite0.tflite"
        }

        try {
            // 使用内存映射 (mmap) 直接加载，杜绝双倍物理内存开销
            objectDetector = ObjectDetector.createFromFileAndOptions(
                context,
                modelName,
                optionsBuilder.build()
            )
            Log.i(TAG, "LiteRT ObjectDetector 初始化成功，载入模型: $modelName")
        } catch (e: IllegalStateException) {
            objectDetectorListener?.onError(
                "LiteRT 引擎初始化失败: ${e.message}。如果为 NNAPI/TPU 模式，请检查驱动兼容性。"
            )
            Log.e(TAG, "LiteRT 引擎初始化失败", e)
        } catch (e: Exception) {
            objectDetectorListener?.onError("模型加载失败: ${e.message}")
            Log.e(TAG, "模型加载失败", e)
        }
    }

    /**
     * 执行实时帧推理
     *
     * @param image 转换后的 Bitmap 图像
     * @param imageRotation 相机传感器的物理旋转校正角度 (0, 90, 180, 270)
     */
    fun detect(image: Bitmap, imageRotation: Int) {
        if (objectDetector == null) {
            setupObjectDetector()
        }

        // 1. 计算传感器角度映射
        val imageProcessorOptions = ImageProcessingOptions.builder()
            .setOrientation(getOrientationFromRotation(imageRotation))
            .build()

        // 2. 构建 TensorImage 输入张量
        val tensorImage = TensorImage.fromBitmap(image)

        // 3. 统计纯硬件推理耗时 (Inference Latency)
        val inferenceStartTime = SystemClock.uptimeMillis()
        val results = objectDetector?.detect(tensorImage, imageProcessorOptions)
        val inferenceTime = SystemClock.uptimeMillis() - inferenceStartTime

        // 4. 回调主线程 UI 渲染
        objectDetectorListener?.onResults(
            results = results ?: emptyList(),
            inferenceTime = inferenceTime,
            imageHeight = tensorImage.height,
            imageWidth = tensorImage.width
        )
    }

    private fun getOrientationFromRotation(rotation: Int): ImageProcessingOptions.Orientation {
        return when (rotation) {
            90 -> ImageProcessingOptions.Orientation.RIGHT_TOP
            180 -> ImageProcessingOptions.Orientation.BOTTOM_RIGHT
            270 -> ImageProcessingOptions.Orientation.LEFT_BOTTOM
            else -> ImageProcessingOptions.Orientation.TOP_LEFT
        }
    }

    interface DetectorListener {
        fun onError(error: String)
        fun onResults(
            results: List<Detection>,
            inferenceTime: Long,
            imageHeight: Int,
            imageWidth: Int
        )
    }

    companion object {
        private const val TAG = "ObjectDetectorHelper"

        const val DELEGATE_CPU = 0
        const val DELEGATE_GPU = 1
        const val DELEGATE_NNAPI = 2

        const val MODEL_EFFICIENTDETV0 = 0
        const val MODEL_MOBILENETV1 = 1
    }
}

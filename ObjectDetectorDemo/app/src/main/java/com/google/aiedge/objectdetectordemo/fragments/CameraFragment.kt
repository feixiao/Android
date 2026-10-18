package com.google.aiedge.objectdetectordemo.fragments

import android.annotation.SuppressLint
import android.content.res.Configuration
import android.graphics.Bitmap
import android.hardware.camera2.CaptureRequest
import android.os.Bundle
import android.util.Log
import android.util.Range
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.camera.camera2.interop.Camera2Interop
import androidx.camera.camera2.interop.ExperimentalCamera2Interop
import androidx.camera.core.AspectRatio
import androidx.camera.core.Camera
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.core.content.ContextCompat
import androidx.fragment.app.Fragment
import androidx.navigation.Navigation
import com.google.aiedge.objectdetectordemo.R
import com.google.aiedge.objectdetectordemo.databinding.FragmentCameraBinding
import com.google.aiedge.objectdetectordemo.helper.ObjectDetectorHelper
import org.tensorflow.lite.task.vision.detector.Detection
import java.util.LinkedList
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.nio.ByteBuffer

/**
 * 核心相机取流与 AI 推理控制器 Fragment
 *
 * 核心职责：
 * 1. CameraX 60fps 实时取流流水线（Camera2Interop 注入 60fps 帧率区间，STRATEGY_KEEP_ONLY_LATEST 零积压）；
 * 2. 图像帧转为 RGBA 格式并直接送入 LiteRT 执行引擎；
 * 3. 屏幕 OverlayView 目标框渲染与底部状态栏（推理延迟 ms、FPS、当前硬件）实时打出；
 * 4. 底部面板交互：实时切换 CPU / GPU / NNAPI TPU 加速模式、调节线程数与置信度。
 */
class CameraFragment : Fragment(), ObjectDetectorHelper.DetectorListener {

    private var _binding: FragmentCameraBinding? = null
    private val binding get() = _binding!!

    private lateinit var objectDetectorHelper: ObjectDetectorHelper
    private lateinit var cameraExecutor: ExecutorService
    private var cameraProvider: ProcessCameraProvider? = null
    private var camera: Camera? = null

    // 帧率测算
    private var frameCount = 0
    private var lastFpsTimestamp = System.currentTimeMillis()
    private var currentFps = 60.0f

    // 复用 Bitmap 与直接缓冲区，避免在分析循环中分配大对象。
    private var bitmapBuffer: Bitmap? = null
    private var rgbaBuffer: ByteBuffer? = null
    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentCameraBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        cameraExecutor = Executors.newSingleThreadExecutor()

        // 1. 初始化 LiteRT 辅助类 (默认开启 Pixel 10 Edge TPU 加速)
        objectDetectorHelper = ObjectDetectorHelper(
            context = requireContext(),
            objectDetectorListener = this,
            currentDelegate = ObjectDetectorHelper.DELEGATE_NNAPI,
            currentThreshold = 0.50f,
            maxResults = 5,
            numThreads = 4,
            currentModel = ObjectDetectorHelper.MODEL_EFFICIENTDETV0
        )

        // 2. 初始化底部控制面板交互
        setupBottomSheetControls()

        // 3. 绑定相机
        binding.viewFinder.post {
            setUpCamera()
        }
    }

    private fun setupBottomSheetControls() {
        val bottomSheet = binding.bottomSheet

        // --- 硬件 Delegate 下拉切换 ---
        val delegateOptions = arrayOf(
            getString(R.string.delegate_nnapi), // 默认选中 NNAPI TPU (Pixel 10)
            getString(R.string.delegate_gpu),
            getString(R.string.delegate_cpu)
        )
        val delegateAdapter = ArrayAdapter(
            requireContext(),
            android.R.layout.simple_spinner_dropdown_item,
            delegateOptions
        )
        bottomSheet.spinnerDelegate.adapter = delegateAdapter
        bottomSheet.spinnerDelegate.setSelection(0)
        bottomSheet.spinnerDelegate.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                val newDelegate = when (position) {
                    0 -> ObjectDetectorHelper.DELEGATE_NNAPI
                    1 -> ObjectDetectorHelper.DELEGATE_GPU
                    else -> ObjectDetectorHelper.DELEGATE_CPU
                }
                if (newDelegate != objectDetectorHelper.currentDelegate) {
                    objectDetectorHelper.currentDelegate = newDelegate
                    updateHardwareUiState()
                    restartDetector()
                }
            }
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        // --- 模型选择下拉 ---
        val modelOptions = arrayOf(
            getString(R.string.model_efficientdet),
            getString(R.string.model_mobilenet)
        )
        val modelAdapter = ArrayAdapter(
            requireContext(),
            android.R.layout.simple_spinner_dropdown_item,
            modelOptions
        )
        bottomSheet.spinnerModel.adapter = modelAdapter
        bottomSheet.spinnerModel.setSelection(0)
        bottomSheet.spinnerModel.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                val newModel = when (position) {
                    0 -> ObjectDetectorHelper.MODEL_EFFICIENTDETV0
                    else -> ObjectDetectorHelper.MODEL_MOBILENETV1
                }
                if (newModel != objectDetectorHelper.currentModel) {
                    objectDetectorHelper.currentModel = newModel
                    restartDetector()
                }
            }
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        // --- CPU 线程数增减 ---
        bottomSheet.btnThreadsMinus.setOnClickListener {
            if (objectDetectorHelper.numThreads > 1) {
                objectDetectorHelper.numThreads--
                bottomSheet.tvThreadsValue.text = objectDetectorHelper.numThreads.toString()
                if (objectDetectorHelper.currentDelegate == ObjectDetectorHelper.DELEGATE_CPU) {
                    restartDetector()
                }
            }
        }
        bottomSheet.btnThreadsPlus.setOnClickListener {
            if (objectDetectorHelper.numThreads < 8) {
                objectDetectorHelper.numThreads++
                bottomSheet.tvThreadsValue.text = objectDetectorHelper.numThreads.toString()
                if (objectDetectorHelper.currentDelegate == ObjectDetectorHelper.DELEGATE_CPU) {
                    restartDetector()
                }
            }
        }

        // --- 置信度阈值增减 ---
        bottomSheet.btnThresholdMinus.setOnClickListener {
            if (objectDetectorHelper.currentThreshold >= 0.15f) {
                objectDetectorHelper.currentThreshold -= 0.05f
                bottomSheet.tvThresholdValue.text = String.format("%.2f", objectDetectorHelper.currentThreshold)
                restartDetector()
            }
        }
        bottomSheet.btnThresholdPlus.setOnClickListener {
            if (objectDetectorHelper.currentThreshold <= 0.85f) {
                objectDetectorHelper.currentThreshold += 0.05f
                bottomSheet.tvThresholdValue.text = String.format("%.2f", objectDetectorHelper.currentThreshold)
                restartDetector()
            }
        }

        // --- 最大检测目标数增减 ---
        bottomSheet.btnResultsMinus.setOnClickListener {
            if (objectDetectorHelper.maxResults > 1) {
                objectDetectorHelper.maxResults--
                bottomSheet.tvResultsValue.text = objectDetectorHelper.maxResults.toString()
                restartDetector()
            }
        }
        bottomSheet.btnResultsPlus.setOnClickListener {
            if (objectDetectorHelper.maxResults < 10) {
                objectDetectorHelper.maxResults++
                bottomSheet.tvResultsValue.text = objectDetectorHelper.maxResults.toString()
                restartDetector()
            }
        }

        updateHardwareUiState()
    }

    private fun updateHardwareUiState() {
        val bottomSheet = binding.bottomSheet
        when (objectDetectorHelper.currentDelegate) {
            ObjectDetectorHelper.DELEGATE_NNAPI -> {
                bottomSheet.tvActiveHardware.text = "Tensor G5 Edge TPU"
                bottomSheet.tvActiveHardware.setTextColor(ContextCompat.getColor(requireContext(), R.color.bounding_box_stroke))
                bottomSheet.layoutThreads.alpha = 0.4f
            }
            ObjectDetectorHelper.DELEGATE_GPU -> {
                bottomSheet.tvActiveHardware.text = "Immortalis GPU (OpenCL)"
                bottomSheet.tvActiveHardware.setTextColor(ContextCompat.getColor(requireContext(), R.color.primary))
                bottomSheet.layoutThreads.alpha = 0.4f
            }
            ObjectDetectorHelper.DELEGATE_CPU -> {
                bottomSheet.tvActiveHardware.text = "ARMv9 CPU (${objectDetectorHelper.numThreads} Threads)"
                bottomSheet.tvActiveHardware.setTextColor(ContextCompat.getColor(requireContext(), R.color.hud_warning))
                bottomSheet.layoutThreads.alpha = 1.0f
            }
        }
    }

    private fun restartDetector() {
        cameraExecutor.execute {
            objectDetectorHelper.clearObjectDetector()
            objectDetectorHelper.setupObjectDetector()
        }
    }

    private fun setUpCamera() {
        val cameraProviderFuture = ProcessCameraProvider.getInstance(requireContext())
        cameraProviderFuture.addListener({
            cameraProvider = cameraProviderFuture.get()
            bindCameraUseCases()
        }, ContextCompat.getMainExecutor(requireContext()))
    }

    @SuppressLint("UnsafeOptInUsageError")
    @OptIn(ExperimentalCamera2Interop::class)
    private fun bindCameraUseCases() {
        val provider = cameraProvider ?: throw IllegalStateException("CameraProvider 未能初始化")

        val cameraSelector = CameraSelector.Builder()
            .requireLensFacing(CameraSelector.LENS_FACING_BACK)
            .build()

        val previewBuilder = Preview.Builder()
            .setTargetAspectRatio(AspectRatio.RATIO_4_3)
            .setTargetRotation(binding.viewFinder.display.rotation)
        val preview = previewBuilder.build()
        // 实际帧率由设备、曝光和绑定的用例共同决定，不在应用层硬编码 60fps。

        // 2. ImageAnalysis (STRATEGY_KEEP_ONLY_LATEST，避免积压旧帧)
        val imageAnalysisBuilder = ImageAnalysis.Builder()
            .setTargetAspectRatio(AspectRatio.RATIO_4_3)
            .setTargetRotation(binding.viewFinder.display.rotation)
            .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
            .setOutputImageFormat(ImageAnalysis.OUTPUT_IMAGE_FORMAT_RGBA_8888)

        val imageAnalysis = imageAnalysisBuilder.build()
        imageAnalysis.setAnalyzer(cameraExecutor) { imageProxy ->
            detectObjects(imageProxy)
        }

        provider.unbindAll()

        try {
            camera = provider.bindToLifecycle(
                viewLifecycleOwner,
                cameraSelector,
                preview,
                imageAnalysis
            )
            preview.setSurfaceProvider(binding.viewFinder.surfaceProvider)
        } catch (exc: Exception) {
            Log.e(TAG, "CameraX 绑定失败", exc)
        }
    }

    private fun detectObjects(imageProxy: ImageProxy) {
        val width = imageProxy.width
        val height = imageProxy.height
        if (bitmapBuffer == null || bitmapBuffer?.width != width || bitmapBuffer?.height != height) {
            bitmapBuffer?.recycle()
            bitmapBuffer = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
            rgbaBuffer = ByteBuffer.allocateDirect(width * height * 4)
        }

        try {
            val plane = imageProxy.planes.firstOrNull()
                ?: throw IllegalArgumentException("RGBA_8888 帧没有可用 plane")
            copyRgbaPlaneToBitmap(plane, width, height, bitmapBuffer!!)
            val rotationDegrees = imageProxy.imageInfo.rotationDegrees

            // 尽早归还 ImageProxy；推理使用的是已复制到的复用 Bitmap。
            imageProxy.close()
            objectDetectorHelper.detect(bitmapBuffer!!, rotationDegrees)
        } catch (error: Exception) {
            imageProxy.close()
            Log.e(TAG, "处理相机帧失败", error)
        }
    }

    /**
     * CameraX 的 RGBA plane 可能包含行尾 padding，不能假定 buffer 是 width * height * 4
     * 的紧凑数组。按 rowStride/pixelStride 逐行复制，同时不修改原 buffer 的 position。
     */
    private fun copyRgbaPlaneToBitmap(
        plane: ImageProxy.PlaneProxy,
        width: Int,
        height: Int,
        target: Bitmap
    ) {
        require(plane.pixelStride >= 4) { "不支持的 RGBA pixelStride=${plane.pixelStride}" }
        val source = plane.buffer.duplicate()
        val rowBytes = width * 4
        val packed = rgbaBuffer ?: ByteBuffer.allocateDirect(width * height * 4).also { rgbaBuffer = it }
        packed.clear()
        for (y in 0 until height) {
            source.position(y * plane.rowStride)
            source.limit(source.position() + rowBytes)
            packed.put(source)
        }
        packed.flip()
        target.copyPixelsFromBuffer(packed)

    }
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        bindCameraUseCases()
    }

    // --- ObjectDetectorHelper.DetectorListener 回调 ---

    override fun onResults(
        results: List<Detection>,
        inferenceTime: Long,
        imageHeight: Int,
        imageWidth: Int
    ) {
        activity?.runOnUiThread {
            if (_binding == null) return@runOnUiThread

            // 1. 刷新 OverlayView 目标框和置信度标签
            binding.overlay.setResults(
                detectionResults = results,
                imageHeight = imageHeight,
                imageWidth = imageWidth
            )

            // 2. 统计计算实时 FPS
            frameCount++
            val now = System.currentTimeMillis()
            val elapsed = now - lastFpsTimestamp
            if (elapsed >= 1000) {
                currentFps = (frameCount * 1000f) / elapsed
                frameCount = 0
                lastFpsTimestamp = now
            }

            // 3. 底部状态栏实时打出推理延迟 (ms)
            binding.bottomSheet.tvInferenceTime.text = String.format("%d ms", inferenceTime)

            // 4. 顶部 HUD 状态栏实时显示
            val hardwareName = when (objectDetectorHelper.currentDelegate) {
                ObjectDetectorHelper.DELEGATE_NNAPI -> "TPU"
                ObjectDetectorHelper.DELEGATE_GPU -> "GPU"
                else -> "CPU"
            }
            binding.tvHudLatency.text = "$hardwareName: ${inferenceTime}ms"
            binding.tvHudFps.text = String.format("%.1f FPS", currentFps)
        }
    }

    override fun onError(error: String) {
        activity?.runOnUiThread {
            Toast.makeText(requireContext(), error, Toast.LENGTH_LONG).show()
        }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        cameraExecutor.shutdown()
        objectDetectorHelper.clearObjectDetector()
        _binding = null
    }

    companion object {
        private const val TAG = "CameraFragment"
    }
}

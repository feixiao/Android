package com.ai.audioagent.manager

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.URL

data class ModelFileInfo(
    val name: String,
    val url: String,
    val approxBytes: Long
)

/**
 * On-Device Model Manager for Full-Duplex AI Audio Agent.
 * Manages Sherpa-onnx Zipformer streaming ASR and VAD model lifecycle.
 */
object ModelManager {
    private const val TAG = "ModelManager"

    // High-speed CDN & HuggingFace direct download URLs
    private val MODEL_FILES = listOf(
        ModelFileInfo(
            name = "encoder-epoch-99-avg-1.int8.onnx",
            url = "https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/main/encoder-epoch-99-avg-1.int8.onnx",
            approxBytes = 21621684L
        ),
        ModelFileInfo(
            name = "decoder-epoch-99-avg-1.onnx",
            url = "https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/main/decoder-epoch-99-avg-1.onnx",
            approxBytes = 7509745L
        ),
        ModelFileInfo(
            name = "joiner-epoch-99-avg-1.int8.onnx",
            url = "https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/main/joiner-epoch-99-avg-1.int8.onnx",
            approxBytes = 1795562L
        ),
        ModelFileInfo(
            name = "tokens.txt",
            url = "https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/main/tokens.txt",
            approxBytes = 48697L
        )
    )

    // High-speed CDN & HuggingFace direct download URLs for Qwen2.5-0.5B-Instruct-MNN
    private val LLM_MODEL_FILES = listOf(
        ModelFileInfo(
            name = "config.json",
            url = "https://huggingface.co/taobao-mnn/Qwen2.5-0.5B-Instruct-MNN/resolve/main/config.json",
            approxBytes = 159L
        ),
        ModelFileInfo(
            name = "llm_config.json",
            url = "https://huggingface.co/taobao-mnn/Qwen2.5-0.5B-Instruct-MNN/resolve/main/llm_config.json",
            approxBytes = 272L
        ),
        ModelFileInfo(
            name = "tokenizer.txt",
            url = "https://huggingface.co/taobao-mnn/Qwen2.5-0.5B-Instruct-MNN/resolve/main/tokenizer.txt",
            approxBytes = 1532454L
        ),
        ModelFileInfo(
            name = "embeddings_bf16.bin",
            url = "https://huggingface.co/taobao-mnn/Qwen2.5-0.5B-Instruct-MNN/resolve/main/embeddings_bf16.bin",
            approxBytes = 135897088L
        ),
        ModelFileInfo(
            name = "llm.mnn",
            url = "https://huggingface.co/taobao-mnn/Qwen2.5-0.5B-Instruct-MNN/resolve/main/llm.mnn",
            approxBytes = 2351234L
        ),
        ModelFileInfo(
            name = "llm.mnn.weight",
            url = "https://huggingface.co/taobao-mnn/Qwen2.5-0.5B-Instruct-MNN/resolve/main/llm.mnn.weight",
            approxBytes = 256897123L
        )
    )
    private val TOTAL_LLM_BYTES = LLM_MODEL_FILES.sumOf { it.approxBytes }

    private val TOTAL_APPROX_BYTES = MODEL_FILES.sumOf { it.approxBytes }

    fun getModelDir(context: Context): File {
        val base = context.getExternalFilesDir("models/asr") ?: File(context.filesDir, "models/asr")
        if (!base.exists()) {
            base.mkdirs()
        }
        return base
    }

    /**
     * Checks if all required on-device ASR models exist and have valid size
     */
    fun isModelReady(context: Context): Boolean {
        val dir = getModelDir(context)
        return MODEL_FILES.all { fileInfo ->
            val f = File(dir, fileInfo.name)
            f.exists() && f.length() > 1024L
        }
    }

    /**
     * Deletes local models
     */
    fun deleteModel(context: Context): Boolean {
        val dir = getModelDir(context)
        var success = true
        for (f in MODEL_FILES) {
            val target = File(dir, f.name)
            if (target.exists()) {
                success = success && target.delete()
            }
        }
        return success
    }

    fun getLlmModelDir(context: Context): File {
        val base = context.getExternalFilesDir("models/llm") ?: File(context.filesDir, "models/llm")
        if (!base.exists()) {
            base.mkdirs()
        }
        return base
    }

    fun isLlmModelReady(context: Context): Boolean {
        val dir = getLlmModelDir(context)
        return LLM_MODEL_FILES.all { fileInfo ->
            val f = File(dir, fileInfo.name)
            f.exists() && f.length() > (if (fileInfo.name.endsWith(".json")) 50L else 500L)
        }
    }

    fun deleteLlmModel(context: Context): Boolean {
        val dir = getLlmModelDir(context)
        var success = true
        for (f in LLM_MODEL_FILES) {
            val target = File(dir, f.name)
            if (target.exists()) {
                success = success && target.delete()
            }
        }
        return success
    }

    suspend fun downloadLlmModels(
        context: Context,
        onProgress: (percent: Int, status: String) -> Unit,
        onComplete: (success: Boolean, message: String) -> Unit
    ) = withContext(Dispatchers.IO) {
        val targetDir = getLlmModelDir(context)
        var totalBytesDownloadedSoFar = 0L

        try {
            for ((index, fileInfo) in LLM_MODEL_FILES.withIndex()) {
                val destFile = File(targetDir, fileInfo.name)
                val tempFile = File(targetDir, "${fileInfo.name}.tmp")

                if (destFile.exists() && destFile.length() >= (if (fileInfo.approxBytes < 1000L) 50L else (fileInfo.approxBytes * 0.8).toLong())) {
                    totalBytesDownloadedSoFar += destFile.length()
                    val pct = ((totalBytesDownloadedSoFar.toDouble() / TOTAL_LLM_BYTES) * 100).toInt().coerceIn(0, 100)
                    withContext(Dispatchers.Main) {
                        onProgress(pct, "已存在 (${index + 1}/${LLM_MODEL_FILES.size}): ${fileInfo.name}")
                    }
                    continue
                }

                withContext(Dispatchers.Main) {
                    val pct = ((totalBytesDownloadedSoFar.toDouble() / TOTAL_LLM_BYTES) * 100).toInt().coerceIn(0, 100)
                    onProgress(pct, "正在下载大模型 (${index + 1}/${LLM_MODEL_FILES.size}): ${fileInfo.name}")
                }

                downloadSingleFile(fileInfo.url, tempFile) { bytesChunk ->
                    totalBytesDownloadedSoFar += bytesChunk
                    val pct = ((totalBytesDownloadedSoFar.toDouble() / TOTAL_LLM_BYTES) * 100).toInt().coerceIn(0, 100)
                    onProgress(pct, "下载大模型中 (${pct}%): ${fileInfo.name}")
                }

                if (tempFile.exists()) {
                    if (destFile.exists()) destFile.delete()
                    tempFile.renameTo(destFile)
                }
            }

            val ready = isLlmModelReady(context)
            withContext(Dispatchers.Main) {
                if (ready) {
                    onComplete(true, "MNN-LLM 真实大模型就绪 (${targetDir.absolutePath})")
                } else {
                    onComplete(false, "MNN-LLM 权重文件校验失败，请重试")
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to download LLM model files", e)
            withContext(Dispatchers.Main) {
                onComplete(false, "大模型下载失败: ${e.localizedMessage}")
            }
        }
    }

    /**
     * Downloads models asynchronously with progress reporting
     */
    suspend fun downloadModels(
        context: Context,
        onProgress: (percent: Int, status: String) -> Unit,
        onComplete: (success: Boolean, message: String) -> Unit
    ) = withContext(Dispatchers.IO) {
        val targetDir = getModelDir(context)
        var totalBytesDownloadedSoFar = 0L

        try {
            for ((index, fileInfo) in MODEL_FILES.withIndex()) {
                val destFile = File(targetDir, fileInfo.name)
                val tempFile = File(targetDir, "${fileInfo.name}.tmp")

                // If file already exists with expected size, skip
                if (destFile.exists() && destFile.length() >= fileInfo.approxBytes * 0.9) {
                    totalBytesDownloadedSoFar += destFile.length()
                    val pct = ((totalBytesDownloadedSoFar.toDouble() / TOTAL_APPROX_BYTES) * 100).toInt().coerceIn(0, 100)
                    withContext(Dispatchers.Main) {
                        onProgress(pct, "已存在 (${index + 1}/${MODEL_FILES.size}): ${fileInfo.name}")
                    }
                    continue
                }

                withContext(Dispatchers.Main) {
                    val pct = ((totalBytesDownloadedSoFar.toDouble() / TOTAL_APPROX_BYTES) * 100).toInt().coerceIn(0, 100)
                    onProgress(pct, "正在下载 (${index + 1}/${MODEL_FILES.size}): ${fileInfo.name}")
                }

                // Download with redirect follow support
                downloadSingleFile(fileInfo.url, tempFile) { bytesChunk ->
                    totalBytesDownloadedSoFar += bytesChunk
                    val pct = ((totalBytesDownloadedSoFar.toDouble() / TOTAL_APPROX_BYTES) * 100).toInt().coerceIn(0, 100)
                    onProgress(pct, "下载中 (${pct}%): ${fileInfo.name}")
                }

                // Rename temp to dest
                if (tempFile.exists()) {
                    if (destFile.exists()) destFile.delete()
                    tempFile.renameTo(destFile)
                }
            }

            val ready = isModelReady(context)
            withContext(Dispatchers.Main) {
                if (ready) {
                    onComplete(true, "离线模型安装就绪 (${targetDir.absolutePath})")
                } else {
                    onComplete(false, "部分模型文件校验失败，请重试")
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to download model files", e)
            withContext(Dispatchers.Main) {
                onComplete(false, "下载失败: ${e.localizedMessage}")
            }
        }
    }

    private fun downloadSingleFile(
        fileUrl: String,
        dest: File,
        onBytesRead: (Long) -> Unit
    ) {
        var currentUrl = fileUrl
        var redirects = 0
        val maxRedirects = 6
        var conn: HttpURLConnection? = null

        while (redirects < maxRedirects) {
            val u = URL(currentUrl)
            conn = (u.openConnection() as HttpURLConnection).apply {
                connectTimeout = 15000
                readTimeout = 30000
                instanceFollowRedirects = false
                setRequestProperty("User-Agent", "AudioAgentDemo/1.0 (Android)")
            }

            val code = conn.responseCode
            if (code in 300..399) {
                val loc = conn.getHeaderField("Location")
                if (!loc.isNullOrEmpty()) {
                    currentUrl = if (loc.startsWith("http")) loc else URL(u, loc).toString()
                    redirects++
                    conn.disconnect()
                    continue
                }
            }
            break
        }

        val connection = conn ?: throw IllegalStateException("Cannot open connection to $fileUrl")
        if (connection.responseCode !in 200..299) {
            throw IllegalStateException("HTTP ${connection.responseCode}: ${connection.responseMessage}")
        }

        var input: InputStream? = null
        var output: FileOutputStream? = null
        try {
            input = connection.inputStream
            output = FileOutputStream(dest)
            val buffer = ByteArray(32 * 1024)
            var bytesRead: Int
            while (input.read(buffer).also { bytesRead = it } != -1) {
                output.write(buffer, 0, bytesRead)
                onBytesRead(bytesRead.toLong())
            }
            output.flush()
        } catch (e: Exception) {
            dest.delete()
            throw e
        } finally {
            output?.close()
            input?.close()
            connection.disconnect()
        }
    }
}

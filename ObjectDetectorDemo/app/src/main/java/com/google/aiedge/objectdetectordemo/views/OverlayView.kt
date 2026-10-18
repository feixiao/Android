package com.google.aiedge.objectdetectordemo.views

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.RectF
import android.util.AttributeSet
import android.view.View
import androidx.core.content.ContextCompat
import com.google.aiedge.objectdetectordemo.R
import org.tensorflow.lite.task.vision.detector.Detection
import java.util.LinkedList
import kotlin.math.max

/**
 * 工业级目标检测实时渲染覆盖层 (OverlayView)
 *
 * 核心机制：
 * 1. 坐标系转换：动态计算 CameraX 传感器取流分辨率与当前屏幕显示分辨率的缩放比例（Scale Factor）；
 * 2. 硬件加速绘制：使用 Canvas 原生绘制圆角矩形边界框 (Bounding Box)、分类名称及置信度标签；
 * 3. 极速重绘：双缓冲机制，杜绝屏幕撕裂与闪烁。
 */
class OverlayView(context: Context?, attrs: AttributeSet?) : View(context, attrs) {

    private var results: List<Detection> = LinkedList<Detection>()
    private var boxPaint = Paint()
    private var textBackgroundPaint = Paint()
    private var textPaint = Paint()

    private var scaleFactor: Float = 1f
    private var bounds = Rect()

    init {
        initPaints()
    }

    fun clear() {
        textPaint.reset()
        textBackgroundPaint.reset()
        boxPaint.reset()
        invalidate()
        initPaints()
    }

    private fun initPaints() {
        textBackgroundPaint.color = Color.parseColor("#CC121212")
        textBackgroundPaint.style = Paint.Style.FILL
        textBackgroundPaint.isAntiAlias = true

        textPaint.color = Color.parseColor("#00E5FF")
        textPaint.style = Paint.Style.FILL
        textPaint.textSize = 42f
        textPaint.isAntiAlias = true
        textPaint.isFakeBoldText = true

        boxPaint.color = ContextCompat.getColor(context!!, R.color.bounding_box_stroke)
        boxPaint.strokeWidth = 8f
        boxPaint.style = Paint.Style.STROKE
        boxPaint.isAntiAlias = true
        boxPaint.strokeCap = Paint.Cap.ROUND
        boxPaint.strokeJoin = Paint.Join.ROUND
    }

    override fun draw(canvas: Canvas) {
        super.draw(canvas)

        for (result in results) {
            val boundingBox = result.boundingBox

            val top = boundingBox.top * scaleFactor
            val bottom = boundingBox.bottom * scaleFactor
            val left = boundingBox.left * scaleFactor
            val right = boundingBox.right * scaleFactor

            // 1. 绘制目标物体的圆角外边框 (高对比度青绿色)
            val drawableRect = RectF(left, top, right, bottom)
            canvas.drawRoundRect(drawableRect, 16f, 16f, boxPaint)

            // 2. 提取类别与置信度文本
            val category = result.categories.firstOrNull() ?: continue
            val labelText = String.format("%s  %.0f%%", category.label, category.score * 100)

            // 3. 计算文本绘制区域背景尺寸
            textPaint.getTextBounds(labelText, 0, labelText.length, bounds)
            val textWidth = bounds.width()
            val textHeight = bounds.height()

            // 4. 绘制半透明标签背景底色
            val tagLeft = left
            val tagTop = top - textHeight - 24f
            val tagRight = left + textWidth + 32f
            val tagBottom = top

            val tagRect = RectF(
                tagLeft.coerceAtLeast(0f),
                tagTop.coerceAtLeast(0f),
                tagRight,
                tagBottom.coerceAtLeast(textHeight.toFloat() + 24f)
            )
            canvas.drawRoundRect(tagRect, 12f, 12f, textBackgroundPaint)

            // 5. 绘制文字标签
            canvas.drawText(
                labelText,
                tagRect.left + 16f,
                tagRect.bottom - 12f,
                textPaint
            )
        }
    }

    /**
     * 接收后台推理结果并触发硬件重绘
     *
     * @param detectionResults LiteRT 检测到的目标物体列表
     * @param imageHeight 相机图像物理高度
     * @param imageWidth 相机图像物理宽度
     */
    fun setResults(
        detectionResults: List<Detection>,
        imageHeight: Int,
        imageWidth: Int
    ) {
        results = detectionResults

        // 计算保持比例的屏幕缩放因数 (Aspect Fill / Fit)
        scaleFactor = max(width * 1f / imageWidth, height * 1f / imageHeight)
        invalidate()
    }
}

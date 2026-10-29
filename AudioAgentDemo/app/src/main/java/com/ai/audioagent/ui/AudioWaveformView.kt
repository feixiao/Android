package com.ai.audioagent.ui

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.AttributeSet
import android.view.View

/**
 * Real-time audio waveform / spectrum visualizer
 */
class AudioWaveformView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    private val barPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#00E676")
        strokeCap = Paint.Cap.ROUND
    }

    private val numBars = 32
    private val amplitudes = FloatArray(numBars) { 0.1f }

    fun updateAmplitudes(data: FloatArray) {
        val step = Math.max(1, data.size / numBars)
        for (i in 0 until numBars) {
            var sum = 0f
            val count = Math.min(step, data.size - i * step)
            for (j in 0 until count) {
                sum += Math.abs(data[i * step + j])
            }
            val avg = if (count > 0) sum / count else 0.05f
            amplitudes[i] = Math.min(1.0f, avg * 3.0f + 0.05f)
        }
        invalidate()
    }

    fun setBarColor(colorInt: Int) {
        barPaint.color = colorInt
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)

        val w = width.toFloat()
        val h = height.toFloat()
        val centerY = h / 2f
        val barWidth = w / (numBars * 2f)
        barPaint.strokeWidth = barWidth

        for (i in 0 until numBars) {
            val x = (i * 2 + 1) * barWidth
            val barHeight = Math.max(8f, amplitudes[i] * (centerY - 10f))
            canvas.drawLine(x, centerY - barHeight, x, centerY + barHeight, barPaint)
        }
    }
}

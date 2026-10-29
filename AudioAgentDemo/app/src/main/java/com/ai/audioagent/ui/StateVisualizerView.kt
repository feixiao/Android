package com.ai.audioagent.ui

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.SweepGradient
import android.util.AttributeSet
import android.view.View
import android.view.animation.LinearInterpolator
import com.ai.audioagent.nativebridge.AgentState

/**
 * Animated UI visualizer rendering the 4 FSM states:
 * IDLE, LISTENING, THINKING, SPEAKING
 */
class StateVisualizerView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    private var currentState: AgentState = AgentState.IDLE
    private var rotationAngle = 0f
    private var pulsePhase = 0f

    private val ringPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 14f
    }

    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
    }

    private val animator = ValueAnimator.ofFloat(0f, 1f).apply {
        duration = 1500
        repeatCount = ValueAnimator.INFINITE
        interpolator = LinearInterpolator()
        addUpdateListener {
            val progress = it.animatedValue as Float
            rotationAngle = progress * 360f
            pulsePhase = (Math.sin(progress * Math.PI * 2).toFloat() + 1f) / 2f
            invalidate()
        }
    }

    init {
        // Started on demand when entering non-IDLE states to conserve CPU & battery
    }

    fun setState(state: AgentState) {
        if (currentState != state) {
            currentState = state
            if (state == AgentState.IDLE) {
                animator.cancel()
            } else {
                if (!animator.isRunning) {
                    animator.start()
                }
            }
            invalidate()
        }
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)

        val cx = width / 2f
        val cy = height / 2f
        val baseRadius = (Math.min(width, height) / 2f) - 30f

        when (currentState) {
            AgentState.IDLE -> {
                // Calm breathing green circle
                val r = baseRadius * (0.85f + 0.05f * pulsePhase)
                fillPaint.color = Color.parseColor("#1B5E20")
                fillPaint.alpha = (40 + 20 * pulsePhase).toInt()
                canvas.drawCircle(cx, cy, r + 15f, fillPaint)

                ringPaint.shader = null
                ringPaint.color = Color.parseColor("#4CAF50")
                canvas.drawCircle(cx, cy, r, ringPaint)
            }

            AgentState.LISTENING -> {
                // Dynamic expanding microphone rings (Cyan/Teal)
                val r1 = baseRadius * (0.8f + 0.15f * pulsePhase)
                fillPaint.color = Color.parseColor("#004D40")
                fillPaint.alpha = (50 + 40 * pulsePhase).toInt()
                canvas.drawCircle(cx, cy, r1, fillPaint)

                ringPaint.shader = null
                ringPaint.color = Color.parseColor("#00E676")
                canvas.drawCircle(cx, cy, r1, ringPaint)

                val r2 = baseRadius * 0.65f
                ringPaint.color = Color.parseColor("#00B0FF")
                canvas.drawCircle(cx, cy, r2, ringPaint)
            }

            AgentState.THINKING -> {
                // Fast rotating gradient ring (Blue/Purple)
                canvas.save()
                canvas.rotate(rotationAngle * 2f, cx, cy)

                val colors = intArrayOf(
                    Color.parseColor("#2979FF"),
                    Color.parseColor("#7C4DFF"),
                    Color.parseColor("#FF4081"),
                    Color.parseColor("#2979FF")
                )
                ringPaint.shader = SweepGradient(cx, cy, colors, null)
                val r = baseRadius * 0.85f
                canvas.drawCircle(cx, cy, r, ringPaint)
                canvas.restore()

                fillPaint.color = Color.parseColor("#304FFE")
                fillPaint.alpha = (40 + 30 * pulsePhase).toInt()
                canvas.drawCircle(cx, cy, baseRadius * 0.7f, fillPaint)
            }

            AgentState.SPEAKING -> {
                // Radiant acoustic soundwave pulses (Orange/Magenta)
                val r1 = baseRadius * (0.85f + 0.15f * pulsePhase)
                val r2 = baseRadius * (0.7f + 0.1f * (1f - pulsePhase))

                fillPaint.color = Color.parseColor("#FF6D00")
                fillPaint.alpha = (50 + 40 * pulsePhase).toInt()
                canvas.drawCircle(cx, cy, r1, fillPaint)

                ringPaint.shader = null
                ringPaint.color = Color.parseColor("#FF9100")
                canvas.drawCircle(cx, cy, r1, ringPaint)

                ringPaint.color = Color.parseColor("#FF4081")
                canvas.drawCircle(cx, cy, r2, ringPaint)
            }
        }
    }

    override fun onDetachedFromWindow() {
        animator.cancel()
        super.onDetachedFromWindow()
    }
}

package org.riddle.diary.input

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.os.Handler
import android.os.Looper
import android.view.MotionEvent
import android.view.View

// Idle-commit delay, ported from main.rs::IDLE_COMMIT: how long the diary
// waits after the last stroke before treating the page as "written" and
// sending it to the oracle.
private const val IDLE_COMMIT_MS = 2800L
private const val ERASE_RADIUS_PX = 40f

// Raw stylus capture surface: pressure + tilt via MotionEvent, S-Pen eraser
// tip erases whole strokes, and an idle timer fires [onCommit] with the page
// once the writer pauses. This replaces the original's raw evdev PenDevice —
// Android's MotionEvent already carries pressure/tilt/tool-type per pointer,
// so there's no need to grab a raw input device.
class StylusCanvasView(context: Context) : View(context) {
    var onCommit: ((List<Stroke>, Bitmap) -> Unit)? = null
    var onInkStarted: (() -> Unit)? = null

    private val strokes = mutableListOf<Stroke>()
    private var current = mutableListOf<StrokePoint>()
    private val livePath = Path()

    private val inkPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.BLACK
        style = Paint.Style.STROKE
        strokeJoin = Paint.Join.ROUND
        strokeCap = Paint.Cap.ROUND
        strokeWidth = 5f
    }

    private val handler = Handler(Looper.getMainLooper())
    private val idleRunnable = Runnable { commitIfAny() }

    init {
        setBackgroundColor(Color.WHITE)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        // Ignore anything that isn't the pen or its eraser tip (palm rejection
        // for finger/mouse input mirrors the original's touch-vs-pen split).
        val tool = event.getToolType(0)
        if (tool != MotionEvent.TOOL_TYPE_STYLUS && tool != MotionEvent.TOOL_TYPE_ERASER) {
            return false
        }

        if (tool == MotionEvent.TOOL_TYPE_ERASER) {
            eraseNear(event.x, event.y)
            resetIdleTimer()
            return true
        }

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                onInkStarted?.invoke()
                current = mutableListOf()
                addSample(event)
                livePath.reset()
                livePath.moveTo(event.x, event.y)
            }
            MotionEvent.ACTION_MOVE -> {
                for (h in 0 until event.historySize) {
                    addSample(event, h)
                    livePath.lineTo(event.getHistoricalX(h), event.getHistoricalY(h))
                }
                addSample(event)
                livePath.lineTo(event.x, event.y)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                addSample(event)
                if (current.size > 1) strokes.add(Stroke(current))
                livePath.reset()
            }
            else -> return false
        }
        resetIdleTimer()
        invalidate()
        return true
    }

    private fun addSample(event: MotionEvent, historyIndex: Int = -1) {
        val x = if (historyIndex >= 0) event.getHistoricalX(historyIndex) else event.x
        val y = if (historyIndex >= 0) event.getHistoricalY(historyIndex) else event.y
        val pressure = if (historyIndex >= 0) event.getHistoricalPressure(historyIndex) else event.pressure
        val tilt = if (historyIndex >= 0) {
            event.getHistoricalAxisValue(MotionEvent.AXIS_TILT, 0, historyIndex)
        } else {
            event.getAxisValue(MotionEvent.AXIS_TILT)
        }
        current.add(StrokePoint(x, y, pressure.coerceIn(0f, 1f), tilt, System.currentTimeMillis()))
    }

    private fun eraseNear(x: Float, y: Float) {
        if (strokes.removeAll { it.boundsIntersects(x, y, ERASE_RADIUS_PX) }) {
            invalidate()
        }
    }

    private fun resetIdleTimer() {
        handler.removeCallbacks(idleRunnable)
        handler.postDelayed(idleRunnable, IDLE_COMMIT_MS)
    }

    private fun commitIfAny() {
        if (strokes.isEmpty()) return
        val bitmap = renderToBitmap()
        val committed = strokes.toList()
        onCommit?.invoke(committed, bitmap)
        strokes.clear()
        invalidate()
    }

    /** Renders the committed page (white background, black ink) as a PNG-ready bitmap. */
    private fun renderToBitmap(): Bitmap {
        val bmp = Bitmap.createBitmap(width.coerceAtLeast(1), height.coerceAtLeast(1), Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)
        canvas.drawColor(Color.WHITE)
        drawStrokes(canvas, strokes)
        return bmp
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        drawStrokes(canvas, strokes)
        canvas.drawPath(livePath, inkPaint)
    }

    private fun drawStrokes(canvas: Canvas, list: List<Stroke>) {
        for (stroke in list) {
            val path = Path()
            val pts = stroke.points
            if (pts.isEmpty()) continue
            path.moveTo(pts[0].x, pts[0].y)
            for (i in 1 until pts.size) path.lineTo(pts[i].x, pts[i].y)
            // Pressure-modulated width: heavier press -> thicker stroke, like the
            // original's 4096-level pressure driving nib width.
            val avgPressure = pts.map { it.pressure }.average().toFloat()
            inkPaint.strokeWidth = 3f + avgPressure * 6f
            canvas.drawPath(path, inkPaint)
        }
        inkPaint.strokeWidth = 5f
    }

    fun clear() {
        handler.removeCallbacks(idleRunnable)
        strokes.clear()
        current = mutableListOf()
        livePath.reset()
        invalidate()
    }
}

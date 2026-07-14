package org.riddle.diary.ink

import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PathMeasure
import android.graphics.Typeface
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.width
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay

// Renders [text] in Dancing Script, "written" stroke-by-stroke (real glyph
// outlines via Paint.getTextPath + PathMeasure — Skia already gives vector
// paths, so this skips the original's rasterize/thin/trace pipeline, which
// only existed to drive the reMarkable's e-ink engine). Holds, then fades.
@Composable
fun HandwritingReply(
    text: String,
    modifier: Modifier = Modifier,
    width: androidx.compose.ui.unit.Dp = 320.dp,
    fontSize: TextUnit = 40.sp,
    msPerChar: Int = 40,
    holdMs: Long = 4000,
    fadeMs: Int = 1500,
    onFinished: () -> Unit = {},
) {
    val context = LocalContext.current
    val density = LocalDensity.current
    val typeface = remember { Typeface.createFromAsset(context.assets, "fonts/DancingScript.ttf") }
    val fontSizePx = with(density) { fontSize.toPx() }
    val maxWidthPx = with(density) { width.toPx() }

    val reveal = remember(text) { Animatable(0f) }
    val alpha = remember(text) { Animatable(1f) }

    val glyphPaint = remember(typeface, fontSizePx) {
        Paint(Paint.ANTI_ALIAS_FLAG).apply {
            this.typeface = typeface
            textSize = fontSizePx
        }
    }

    val fullPath = remember(text, fontSizePx) { buildReplyPath(text, glyphPaint, maxWidthPx) }
    val totalLength = remember(fullPath) { pathLength(fullPath) }

    LaunchedEffect(text) {
        reveal.snapTo(0f)
        alpha.snapTo(1f)
        if (text.isBlank()) return@LaunchedEffect
        val duration = (text.length * msPerChar).coerceIn(400, 6000)
        reveal.animateTo(1f, animationSpec = tween(durationMillis = duration, easing = LinearEasing))
        delay(holdMs)
        alpha.animateTo(0f, animationSpec = tween(durationMillis = fadeMs))
        onFinished()
    }

    Canvas(modifier = modifier.width(width)) {
        if (text.isBlank()) return@Canvas
        val dst = revealedSegment(fullPath, totalLength * reveal.value)
        val inkPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE
            strokeWidth = 3f
            strokeCap = Paint.Cap.ROUND
            strokeJoin = Paint.Join.ROUND
            color = Color.argb((alpha.value * 255).toInt(), 20, 20, 40)
        }
        drawIntoCanvas { canvas -> canvas.nativeCanvas.drawPath(dst, inkPaint) }
    }
}

private fun buildReplyPath(text: String, paint: Paint, maxWidthPx: Float): Path {
    val lineHeight = paint.fontSpacing
    val full = Path()
    wrapText(text, paint, maxWidthPx).forEachIndexed { i, line ->
        val linePath = Path()
        paint.getTextPath(line, 0, line.length, 0f, (i + 1) * lineHeight, linePath)
        full.addPath(linePath)
    }
    return full
}

private fun wrapText(text: String, paint: Paint, maxWidth: Float): List<String> {
    val lines = mutableListOf<String>()
    for (para in text.split("\n")) {
        var cur = ""
        for (word in para.split(" ")) {
            if (word.isEmpty()) continue
            val candidate = if (cur.isEmpty()) word else "$cur $word"
            cur = if (paint.measureText(candidate) <= maxWidth || cur.isEmpty()) {
                candidate
            } else {
                lines.add(cur)
                word
            }
        }
        if (cur.isNotEmpty()) lines.add(cur)
    }
    return lines
}

private fun pathLength(path: Path): Float {
    val measure = PathMeasure(path, false)
    var total = 0f
    do {
        total += measure.length
    } while (measure.nextContour())
    return total
}

private fun revealedSegment(path: Path, revealLength: Float): Path {
    val dst = Path()
    val measure = PathMeasure(path, false)
    var consumed = 0f
    do {
        val segLen = measure.length
        if (consumed < revealLength) {
            val take = (revealLength - consumed).coerceAtMost(segLen)
            measure.getSegment(0f, take, dst, true)
        }
        consumed += segLen
    } while (measure.nextContour())
    return dst
}

package org.riddle.diary.input

// A single recorded pen sample. Pressure is normalized 0f..1f (Android already
// scales this per-device, unlike the original's raw 0..4096 digitizer value).
data class StrokePoint(
    val x: Float,
    val y: Float,
    val pressure: Float,
    val tilt: Float,
    val tMillis: Long,
)

data class Stroke(val points: List<StrokePoint>) {
    fun boundsIntersects(px: Float, py: Float, radius: Float): Boolean =
        points.any { p -> val dx = p.x - px; val dy = p.y - py; dx * dx + dy * dy <= radius * radius }
}

package org.riddle.diary.memory

import java.time.Instant
import java.time.ZoneId
import java.time.format.TextStyle
import java.util.Locale
import kotlinx.serialization.Serializable
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.riddle.diary.input.Stroke

// Ported from the original riddle's src/memory.rs, backed by Room instead of
// flat files. Every finished turn is kept — transcript, reply, and decimated
// pen strokes — so a later "show me what I wrote about…" can conjure the page.
private const val MAX_MEMORIES = 400

// Drop replay points closer than this (px^2) to the last kept one.
private const val MIN_POINT_DIST2 = 9L

@Serializable
data class StoredPoint(val x: Int, val y: Int, val r: Int)

data class MemoryEntry(val id: Long, val transcript: String, val reply: String)

class MemoryStore(private val dao: MemoryDao) {

    suspend fun append(id: Long, transcript: String, reply: String, strokes: List<Stroke>) {
        val json = Json.encodeToString(decimate(strokes))
        dao.insert(MemoryEntity(id = id, transcript = transcript, reply = reply, strokesJson = json))
        prune()
    }

    private suspend fun prune() {
        val count = dao.count()
        if (count > MAX_MEMORIES) dao.deleteOldest(count - MAX_MEMORIES)
    }

    suspend fun get(id: Long): MemoryEntry? = dao.get(id)?.let { MemoryEntry(it.id, it.transcript, it.reply) }

    suspend fun strokes(id: Long): List<List<StoredPoint>>? =
        dao.get(id)?.strokesJson?.let { Json.decodeFromString(it) }

    // Last n turns, oldest first, skipping blank transcripts — the
    // conversational memory that rides along with each request.
    suspend fun recentDialogue(n: Int): List<Pair<String, String>> =
        dao.all().filter { it.transcript.isNotBlank() }.takeLast(n).map { it.transcript to it.reply }

    // Numbered newest-first catalog + parallel ids; catalogIds[i] belongs to
    // catalog number i+1, matching TurnContext's contract.
    suspend fun catalog(max: Int): Pair<List<String>, List<Long>> {
        val newestFirst = dao.all().asReversed().take(max)
        val lines = newestFirst.mapIndexed { i, e ->
            val gist = if (e.transcript.isBlank()) "(reply: ${oneLine(e.reply, 70)})" else oneLine(e.transcript, 70)
            "${i + 1}. ${spokenDate(e.id)} — $gist"
        }
        return lines to newestFirst.map { it.id }
    }
}

private fun oneLine(s: String, max: Int): String =
    s.split(Regex("\\s+")).filter { it.isNotEmpty() }.joinToString(" ").take(max)

private fun decimate(strokes: List<Stroke>): List<List<StoredPoint>> = strokes.map { stroke ->
    val out = mutableListOf<StoredPoint>()
    val pts = stroke.points
    pts.forEachIndexed { i, p ->
        // No raw 0..4096 digitizer pressure on Android; approximate the
        // original's stroke radius from the normalized 0f..1f pressure.
        val r = (p.pressure * 8).toInt().coerceAtLeast(1)
        val keep = out.lastOrNull()?.let { last ->
            val dx = (p.x.toInt() - last.x).toLong()
            val dy = (p.y.toInt() - last.y).toLong()
            dx * dx + dy * dy >= MIN_POINT_DIST2 || i == pts.size - 1
        } ?: true
        if (keep) out.add(StoredPoint(p.x.toInt(), p.y.toInt(), r))
    }
    out
}.filter { it.isNotEmpty() }

// "the 6th of July, in the evening" — how the diary speaks of a moment. Uses
// the device's real timezone (java.time), unlike the original's RIDDLE_TZ_OFFSET
// workaround for the reMarkable's unreliable clock.
fun spokenDate(epochSeconds: Long): String {
    val zdt = Instant.ofEpochSecond(epochSeconds).atZone(ZoneId.systemDefault())
    val day = zdt.dayOfMonth
    val suffix = when {
        day in 11..13 -> "th"
        day % 10 == 1 -> "st"
        day % 10 == 2 -> "nd"
        day % 10 == 3 -> "rd"
        else -> "th"
    }
    val month = zdt.month.getDisplayName(TextStyle.FULL, Locale.ENGLISH)
    val tod = when (zdt.hour) {
        in 0..4 -> "in the small hours"
        in 5..11 -> "in the morning"
        in 12..17 -> "in the afternoon"
        in 18..21 -> "in the evening"
        else -> "late at night"
    }
    return "the $day$suffix of $month, $tod"
}

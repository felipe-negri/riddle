package org.riddle.diary.llm

private const val SENTINEL = '⁂' // ⁂
private const val SHOW_OPEN = '⟦' // ⟦
private const val SHOW_CLOSE = '⟧' // ⟧

/**
 * Incremental parser over the model's streamed text, ported from oracle.rs::StreamParser.
 * Routes the ⟦show:N⟧ directive, chunks prose into sentences, and splits off the
 * ⁂-transcription postscript. Fed the RUNNING full text (providers accumulate deltas),
 * it emits each event exactly once. [advance] is NOT thread-safe; call it from one coroutine.
 */
class StreamParser(private val catalogIds: List<Long>) {
    private var delivered = 0
    private var sentinel: Int? = null
    private var routeChecked = false
    private var emittedAny = false

    // Feed the full accumulated reply text so far. `done` marks end of stream:
    // flushes the tail and the transcription.
    fun advance(full: String, done: Boolean): List<OracleEvent> {
        val out = mutableListOf<OracleEvent>()

        if (sentinel == null) {
            val idx = full.indexOf(SENTINEL)
            if (idx >= 0) sentinel = idx
        }
        // The reply body is everything before the ⁂ transcription postscript.
        val effective = sentinel ?: full.length

        // Route: is this reply an incantation (⟦show:N⟧) rather than prose? The
        // model is told the directive must stand alone, so it's honored only
        // when it LEADS the reply. Output is held until the lead is settled:
        // either the directive appears (honor it) or real prose does (normal
        // reply). This can't un-ink, so a directive is only honored before any
        // prose has streamed.
        if (!routeChecked) {
            val lead = full.substring(delivered, effective).trimStart()
            if (lead.startsWith(SHOW_OPEN)) {
                val closeRel = lead.indexOf(SHOW_CLOSE)
                if (closeRel < 0) {
                    if (!done) return out // directive still streaming in
                    out.add(OracleEvent.Error("unfinished conjuring directive"))
                    return out
                }
                val inner = lead.substring(1, closeRel)
                val n = parseShowNumber(inner)
                routeChecked = true
                emittedAny = true
                delivered = effective // consume the whole body
                val id = n?.let { idx -> catalogIds.getOrNull(idx - 1) }
                out.add(if (id != null) OracleEvent.Show(id) else OracleEvent.Error("the diary lost that page ($inner)"))
            } else if (lead.isEmpty()) {
                if (!done) return out // only whitespace so far — keep waiting
                routeChecked = true
            } else {
                // Real prose leads: a normal reply.
                routeChecked = true
            }
        }

        // Prose sentences, never crossing into the transcription postscript. A
        // stray directive that appears AFTER prose (a misbehaving model) is
        // stripped here so the writer never sees ⟦…⟧ glyphs inked.
        if (delivered < effective) {
            val cut = sentenceCut(full.substring(0, effective), delivered)
            if (cut != null) {
                val chunk = stripDirectives(clean(full.substring(delivered, cut)))
                if (chunk.isNotEmpty()) {
                    emittedAny = true
                    out.add(OracleEvent.Ink(chunk))
                }
                delivered = cut
            }
        }

        if (done) {
            if (delivered < effective) {
                val rest = stripDirectives(clean(full.substring(delivered, effective).trim()))
                if (rest.isNotEmpty()) {
                    emittedAny = true
                    out.add(OracleEvent.Ink(rest))
                }
                delivered = effective
            }
            sentinel?.let { p ->
                val t = full.substring(p + 1).trim()
                if (t.isNotEmpty()) {
                    out.add(OracleEvent.Transcript(t))
                }
            }
            if (!emittedAny) {
                out.add(OracleEvent.Error("empty reply"))
            }
        }
        return out
    }
}

// Parses the inner text of a ⟦…⟧ directive into a 1-based catalog number, e.g.
// "show:2" / "Show: 2" / "SHOW 2" -> 2. Returns null if it isn't a "show" directive.
private fun parseShowNumber(inner: String): Int? {
    val lower = inner.lowercase()
    if (!lower.startsWith("show")) return null
    return lower.substring(4).trimStart(':', ' ').trim().toIntOrNull()
}

// End of the LAST complete sentence in `text` after offset `from`: sentence
// punctuation followed by whitespace or end-of-text, at least 4 chars in.
// Returns the offset just past the punctuation, or null if none completed.
private fun sentenceCut(text: String, from: Int): Int? {
    if (from > text.length) return null
    val tail = text.substring(from)
    var cut: Int? = null
    for (i in tail.indices) {
        val c = tail[i]
        if (c == '.' || c == '!' || c == '?' || c == '…') {
            val end = i + 1
            val next = tail.getOrNull(end)
            if ((next == null || next.isWhitespace()) && end >= 4) {
                cut = from + end
            }
        }
    }
    return cut
}

// Trim and strip stray surrounding quotes from a reply fragment.
private fun clean(s: String): String {
    var t = s.trim()
    if (t.startsWith('"')) t = t.substring(1)
    if (t.endsWith('"')) t = t.substring(0, t.length - 1)
    return t
}

// Remove any ⟦…⟧ directive spans from inked prose, so a misbehaving model that
// emits a directive mid/after prose never renders ⟦…⟧ as literal glyphs. (A
// directive that LEADS the reply is routed earlier, in advance().)
private fun stripDirectives(s: String): String {
    if (!s.contains(SHOW_OPEN)) return s
    val out = StringBuilder()
    var rest = s
    while (true) {
        val open = rest.indexOf(SHOW_OPEN)
        if (open < 0) {
            out.append(rest)
            break
        }
        out.append(rest, 0, open)
        val close = rest.indexOf(SHOW_CLOSE, open)
        if (close < 0) {
            break // unterminated: drop the tail
        }
        rest = rest.substring(close + 1)
    }
    return out.toString().split(Regex("\\s+")).filter { it.isNotEmpty() }.joinToString(" ")
}

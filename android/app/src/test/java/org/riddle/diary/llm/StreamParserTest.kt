package org.riddle.diary.llm

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

// Mirrors the test suite in the original riddle's src/oracle.rs (StreamParser tests).
class StreamParserTest {

    private fun ok(events: List<OracleEvent>) = events.also {
        assertTrue("expected no errors, got $it", it.none { e -> e is OracleEvent.Error })
    }

    @Test
    fun parserStreamsProseThenTranscript() {
        val p = StreamParser(emptyList())
        assertTrue(p.advance("Hello", false).isEmpty())
        val ev1 = ok(p.advance("Hello. Who wri", false))
        assertEquals(listOf(OracleEvent.Ink("Hello.")), ev1)
        val full = "Hello. Who writes to me? ⁂ it rained all night"
        val ev2 = ok(p.advance(full, true))
        assertEquals(
            listOf(
                OracleEvent.Ink("Who writes to me?"),
                OracleEvent.Transcript("it rained all night"),
            ),
            ev2,
        )
    }

    @Test
    fun parserRoutesShowDirective() {
        val p = StreamParser(listOf(900L, 800L, 700L))
        assertTrue(p.advance("⟦sho", false).isEmpty())
        val ev1 = ok(p.advance("⟦show:2⟧", false))
        assertEquals(listOf(OracleEvent.Show(800L)), ev1)
        val full = "⟦show:2⟧\n⁂ show me the garden page"
        val ev2 = ok(p.advance(full, true))
        assertEquals(listOf(OracleEvent.Transcript("show me the garden page")), ev2)
    }

    @Test
    fun parserShowToleratesSpacingAndCase() {
        val p = StreamParser(listOf(42L))
        val ev = p.advance("  ⟦Show: 1⟧", true)
        assertTrue(ev.toString(), ev.contains(OracleEvent.Show(42L)))
    }

    @Test
    fun parserShowOutOfRangeIsError() {
        val p = StreamParser(listOf(42L))
        val ev = p.advance("⟦show:7⟧", true)
        assertTrue(ev[0] is OracleEvent.Error)
    }

    @Test
    fun parserEmptyReplyIsError() {
        val p = StreamParser(emptyList())
        val ev = p.advance("", true)
        assertTrue(ev[0] is OracleEvent.Error)
    }

    @Test
    fun parserWithoutSentinelStillFlushes() {
        val p = StreamParser(emptyList())
        val ev = ok(p.advance("A reply without postscript", true))
        assertEquals(listOf(OracleEvent.Ink("A reply without postscript")), ev)
    }

    @Test
    fun parserLeadingDirectiveConjuresAndTakesTheWholeBody() {
        val p = StreamParser(listOf(900L, 800L))
        val full = "⟦show:2⟧\n⁂ show me the rain"
        val ev = ok(p.advance(full, true))
        assertEquals(
            listOf(OracleEvent.Show(800L), OracleEvent.Transcript("show me the rain")),
            ev,
        )
    }

    @Test
    fun parserDirectiveAfterProseIsStrippedNotInked() {
        val p = StreamParser(listOf(900L, 800L))
        val full = "Of course, let me show you. ⟦show:2⟧\n⁂ show me the rain"
        val ev = ok(p.advance(full, true))
        assertEquals(
            listOf(
                OracleEvent.Ink("Of course, let me show you."),
                OracleEvent.Transcript("show me the rain"),
            ),
            ev,
        )
        assertTrue(ev.none { it is OracleEvent.Ink && it.text.contains('⟦') })
    }
}

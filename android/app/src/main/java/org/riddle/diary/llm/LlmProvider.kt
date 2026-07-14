package org.riddle.diary.llm

import kotlinx.coroutines.flow.Flow

// What a turn carries besides the page image/transcript: the diary's memory.
data class TurnContext(
    // Recent (transcript, reply) pairs, oldest first.
    val history: List<Pair<String, String>> = emptyList(),
    // Catalog lines shown to the model ("1. the 6th of July… — gist").
    val catalogLines: List<String> = emptyList(),
    // catalogIds[i] is the memory id behind catalog number i+1.
    val catalogIds: List<Long> = emptyList(),
)

// What the oracle streams back to the diary.
sealed class OracleEvent {
    data class Ink(val text: String) : OracleEvent()
    data class Show(val memoryId: Long) : OracleEvent()
    data class Transcript(val text: String) : OracleEvent()
    data class Error(val message: String) : OracleEvent()
}

// A single turn's input to a provider: the page as strokes-recognized text
// and/or an image, depending on what the model and recognizer can offer.
data class TurnInput(
    val transcript: String?,
    val imagePng: ByteArray?,
)

interface LlmProvider {
    val supportsVision: Boolean

    fun ask(input: TurnInput, ctx: TurnContext, remember: Boolean): Flow<OracleEvent>
}

// The per-turn user text: memory catalog (when remembering) + instruction.
// Ported from oracle.rs::turn_text.
fun turnText(ctx: TurnContext): String {
    if (ctx.catalogLines.isEmpty()) {
        return "Reply to what is written in the diary."
    }
    return "Memory catalog (newest first):\n" +
        ctx.catalogLines.joinToString("\n") +
        "\n\nReply to what is written in the diary."
}

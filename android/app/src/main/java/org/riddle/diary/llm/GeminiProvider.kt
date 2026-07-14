package org.riddle.diary.llm

import android.util.Base64
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.addJsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonArray
import kotlinx.serialization.json.putJsonObject
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import okhttp3.sse.EventSource
import okhttp3.sse.EventSourceListener
import okhttp3.sse.EventSources
import java.util.concurrent.TimeUnit

// Gemini's `streamGenerateContent` with `alt=sse`. Ported alongside
// oracle.rs's HttpOracle but with Gemini's contents/parts request shape.
data class GeminiConfig(
    val baseUrl: String = "https://generativelanguage.googleapis.com",
    val apiKey: String,
    val model: String = "gemini-2.0-flash",
)

class GeminiProvider(private val config: GeminiConfig) : LlmProvider {
    override val supportsVision = true

    private val client = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(90, TimeUnit.SECONDS)
        .build()

    override fun ask(input: TurnInput, ctx: TurnContext, remember: Boolean): Flow<OracleEvent> = callbackFlow {
        val system = systemPrompt(remember)
        val userText = turnText(ctx)
        val imageB64 = input.imagePng?.let { Base64.encodeToString(it, Base64.NO_WRAP) }

        val body = buildJsonObject {
            putJsonObject("system_instruction") {
                putJsonArray("parts") { addJsonObject { put("text", system) } }
            }
            putJsonArray("contents") {
                for ((transcript, reply) in ctx.history) {
                    addJsonObject {
                        put("role", "user")
                        putJsonArray("parts") { addJsonObject { put("text", "(an earlier page) $transcript") } }
                    }
                    addJsonObject {
                        put("role", "model")
                        putJsonArray("parts") { addJsonObject { put("text", reply) } }
                    }
                }
                addJsonObject {
                    put("role", "user")
                    putJsonArray("parts") {
                        addJsonObject { put("text", userText) }
                        if (imageB64 != null) {
                            addJsonObject {
                                putJsonObject("inline_data") {
                                    put("mime_type", "image/png")
                                    put("data", imageB64)
                                }
                            }
                        }
                    }
                }
            }
        }.toString()

        val url = "${config.baseUrl.trimEnd('/')}/v1beta/models/${config.model}:streamGenerateContent" +
            "?alt=sse&key=${config.apiKey}"
        val request = Request.Builder()
            .url(url)
            .header("Content-Type", "application/json")
            .post(body.toRequestBody("application/json".toMediaType()))
            .build()

        val parser = StreamParser(ctx.catalogIds)
        val acc = StringBuilder()

        val listener = object : EventSourceListener() {
            override fun onEvent(eventSource: EventSource, id: String?, type: String?, data: String) {
                val frag = geminiDeltaText(data) ?: return
                if (frag.isEmpty()) return
                acc.append(frag)
                for (e in parser.advance(acc.toString(), false)) trySend(e)
            }

            override fun onClosed(eventSource: EventSource) {
                for (e in parser.advance(acc.toString(), true)) trySend(e)
                close()
            }

            override fun onFailure(eventSource: EventSource, t: Throwable?, response: Response?) {
                val detail = response?.body?.string()
                response?.close()
                val msg = when {
                    response != null -> "http ${response.code}: ${detail?.trim().orEmpty()}"
                    t != null -> "request failed: ${t.message}"
                    else -> "request failed"
                }
                trySend(OracleEvent.Error(msg))
                close()
            }
        }

        val source = EventSources.createFactory(client).newEventSource(request, listener)
        awaitClose { source.cancel() }
    }
}

// `{"candidates":[{"content":{"parts":[{"text":"..."}]}}]}`
private fun geminiDeltaText(data: String): String? = try {
    val root = Json.parseToJsonElement(data).jsonObject
    val parts = root["candidates"]?.jsonArray?.getOrNull(0)?.jsonObject
        ?.get("content")?.jsonObject?.get("parts")?.jsonArray
    parts?.joinToString("") { it.jsonObject["text"]?.jsonPrimitive?.contentOrNull.orEmpty() }
} catch (e: Exception) {
    null
}

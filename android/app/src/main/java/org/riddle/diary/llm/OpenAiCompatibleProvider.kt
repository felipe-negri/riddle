package org.riddle.diary.llm

import android.util.Base64
import android.util.Log
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

private const val TAG = "OpenAiProvider"

// Any OpenAI-compatible `/chat/completions` endpoint: OpenAI, OpenRouter, Groq, LM
// Studio, a local server, or Ollama's own `/v1` shim. Ported from oracle.rs::HttpOracle.
data class OpenAiConfig(
    val baseUrl: String = "https://api.openai.com/v1",
    val apiKey: String,
    val model: String = "gpt-4o-mini",
    val maxTokens: Int = 2000,
    // Sent as "reasoning_effort" only when set — some providers reject the field
    // on non-reasoning models, so it must stay out of the default request.
    val reasoningEffort: String? = null,
)

class OpenAiCompatibleProvider(private val config: OpenAiConfig) : LlmProvider {
    override val supportsVision = true

    private val client = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        // Per-read timeout, not total: a healthy stream can run long, thinking
        // models can lead with ~a minute of silence — only true silence trips it.
        .readTimeout(90, TimeUnit.SECONDS)
        .build()

    override fun ask(input: TurnInput, ctx: TurnContext, remember: Boolean): Flow<OracleEvent> = callbackFlow {
        val system = systemPrompt(remember)
        val userText = turnText(ctx)
        val imageB64 = input.imagePng?.let { Base64.encodeToString(it, Base64.NO_WRAP) }
        val base = config.baseUrl.trimEnd('/')

        fun body(capField: String): String = buildJsonObject {
            put("model", config.model)
            put("stream", true)
            put(capField, config.maxTokens)
            config.reasoningEffort?.let { put("reasoning_effort", it) }
            putJsonArray("messages") {
                addJsonObject { put("role", "system"); put("content", system) }
                for ((transcript, reply) in ctx.history) {
                    addJsonObject { put("role", "user"); put("content", "(an earlier page) $transcript") }
                    addJsonObject { put("role", "assistant"); put("content", reply) }
                }
                addJsonObject {
                    put("role", "user")
                    putJsonArray("content") {
                        addJsonObject { put("type", "text"); put("text", userText) }
                        if (imageB64 != null) {
                            addJsonObject {
                                put("type", "image_url")
                                putJsonObject("image_url") { put("url", "data:image/png;base64,$imageB64") }
                            }
                        }
                    }
                }
            }
        }.toString()

        val parser = StreamParser(ctx.catalogIds)
        val acc = StringBuilder()

        fun emitAll(events: List<OracleEvent>) {
            for (e in events) trySend(e)
        }

        fun request(capField: String): Request = Request.Builder()
            .url("$base/chat/completions")
            .header("Authorization", "Bearer ${config.apiKey}")
            .header("Content-Type", "application/json")
            .post(body(capField).toRequestBody("application/json".toMediaType()))
            .build()

        var retriedCapField = false

        val listener = object : EventSourceListener() {
            override fun onEvent(eventSource: EventSource, id: String?, type: String?, data: String) {
                if (data == "[DONE]") return
                val frag = sseDeltaContent(data) ?: return
                if (frag.isEmpty()) return
                acc.append(frag)
                emitAll(parser.advance(acc.toString(), false))
            }

            override fun onClosed(eventSource: EventSource) {
                emitAll(parser.advance(acc.toString(), true))
                close()
            }

            override fun onFailure(eventSource: EventSource, t: Throwable?, response: Response?) {
                // OpenAI's newest models reject "max_tokens" and demand
                // "max_completion_tokens"; many OpenAI-compatible servers only
                // know "max_tokens". Try the widely-supported name first,
                // retry once if the server corrects us.
                val detail = response?.body?.string()
                response?.close()
                if (!retriedCapField && response?.code == 400 && detail?.contains("max_completion_tokens") == true) {
                    retriedCapField = true
                    Log.i(TAG, "endpoint wants max_completion_tokens; retrying")
                    EventSources.createFactory(client).newEventSource(request("max_completion_tokens"), this)
                    return
                }
                val msg = when {
                    response != null -> "http ${response.code}: ${detail?.trim().orEmpty()}"
                    t != null -> "request failed: ${t.message}"
                    else -> "request failed"
                }
                trySend(OracleEvent.Error(msg))
                close()
            }
        }

        val source = EventSources.createFactory(client).newEventSource(request("max_tokens"), listener)
        awaitClose { source.cancel() }
    }
}

// Pull `choices[0].delta.content` out of one SSE `data:` JSON object.
private fun sseDeltaContent(data: String): String? = try {
    val root = Json.parseToJsonElement(data).jsonObject
    val delta = root["choices"]?.jsonArray?.getOrNull(0)?.jsonObject?.get("delta")?.jsonObject
    delta?.get("content")?.jsonPrimitive?.contentOrNull
} catch (e: Exception) {
    null
}

package org.riddle.diary.llm

import android.util.Base64
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.addJsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
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

// Anthropic's native `/v1/messages`, streamed as SSE. Ported alongside
// oracle.rs's HttpOracle but with Claude's request/event shape.
data class AnthropicConfig(
    val baseUrl: String = "https://api.anthropic.com",
    val apiKey: String,
    val model: String = "claude-3-5-sonnet-20241022",
    val maxTokens: Int = 2000,
    val apiVersion: String = "2023-06-01",
)

class AnthropicProvider(private val config: AnthropicConfig) : LlmProvider {
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
            put("model", config.model)
            put("max_tokens", config.maxTokens)
            put("stream", true)
            put("system", system)
            putJsonArray("messages") {
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
                                put("type", "image")
                                putJsonObject("source") {
                                    put("type", "base64")
                                    put("media_type", "image/png")
                                    put("data", imageB64)
                                }
                            }
                        }
                    }
                }
            }
        }.toString()

        val request = Request.Builder()
            .url("${config.baseUrl.trimEnd('/')}/v1/messages")
            .header("x-api-key", config.apiKey)
            .header("anthropic-version", config.apiVersion)
            .header("content-type", "application/json")
            .post(body.toRequestBody("application/json".toMediaType()))
            .build()

        val parser = StreamParser(ctx.catalogIds)
        val acc = StringBuilder()

        val listener = object : EventSourceListener() {
            override fun onEvent(eventSource: EventSource, id: String?, type: String?, data: String) {
                when (type) {
                    "content_block_delta" -> {
                        val frag = anthropicDeltaText(data) ?: return
                        if (frag.isEmpty()) return
                        acc.append(frag)
                        for (e in parser.advance(acc.toString(), false)) trySend(e)
                    }
                    "message_stop" -> {
                        for (e in parser.advance(acc.toString(), true)) trySend(e)
                        close()
                    }
                    "error" -> {
                        trySend(OracleEvent.Error("anthropic stream error: $data"))
                        close()
                    }
                }
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

// `{"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"..."}}`
private fun anthropicDeltaText(data: String): String? = try {
    val root = Json.parseToJsonElement(data).jsonObject
    root["delta"]?.jsonObject?.get("text")?.jsonPrimitive?.contentOrNull
} catch (e: Exception) {
    null
}

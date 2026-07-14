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

// EXPERIMENTAL: `api.githubcopilot.com/chat/completions` is undocumented and
// only reachable with a Copilot subscription behind the token this provider's
// [CopilotTokenProvider] obtains via [CopilotDeviceFlow]. Shape mirrors
// OpenAI's `/chat/completions`, but with Copilot's required extra headers.
// May break without notice; keep isolated from the documented providers.
data class CopilotConfig(
    val model: String = "gpt-4o",
)

class CopilotProvider(
    private val tokens: CopilotTokenProvider,
    private val config: CopilotConfig = CopilotConfig(),
) : LlmProvider {
    override val supportsVision = true

    private val client = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(90, TimeUnit.SECONDS)
        .build()

    override fun ask(input: TurnInput, ctx: TurnContext, remember: Boolean): Flow<OracleEvent> = callbackFlow {
        val system = systemPrompt(remember)
        val userText = turnText(ctx)
        val imageB64 = input.imagePng?.let { Base64.encodeToString(it, Base64.NO_WRAP) }
        val token = try {
            tokens.token()
        } catch (e: Exception) {
            trySend(OracleEvent.Error("copilot sign-in required: ${e.message}"))
            close()
            return@callbackFlow
        }

        val body = buildJsonObject {
            put("model", config.model)
            put("stream", true)
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

        val request = Request.Builder()
            .url("https://api.githubcopilot.com/chat/completions")
            .header("Authorization", "Bearer $token")
            .header("Content-Type", "application/json")
            .header("Copilot-Integration-Id", "vscode-chat")
            .header("Editor-Version", "vscode/1.90.0")
            .header("Editor-Plugin-Version", "copilot-chat/0.12.0")
            .post(body.toRequestBody("application/json".toMediaType()))
            .build()

        val parser = StreamParser(ctx.catalogIds)
        val acc = StringBuilder()

        val listener = object : EventSourceListener() {
            override fun onEvent(eventSource: EventSource, id: String?, type: String?, data: String) {
                if (data == "[DONE]") return
                val frag = copilotDeltaContent(data) ?: return
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

private fun copilotDeltaContent(data: String): String? = try {
    val root = Json.parseToJsonElement(data).jsonObject
    root["choices"]?.jsonArray?.getOrNull(0)?.jsonObject?.get("delta")?.jsonObject
        ?.get("content")?.jsonPrimitive?.contentOrNull
} catch (e: Exception) {
    null
}

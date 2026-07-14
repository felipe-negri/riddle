package org.riddle.diary.llm

import android.util.Base64
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.add
import kotlinx.serialization.json.addJsonObject
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonArray
import okhttp3.Call
import okhttp3.Callback
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import java.io.IOException
import java.util.concurrent.TimeUnit

// A local (or LAN) Ollama server's native `/api/chat`, streamed as NDJSON (one
// JSON object per line, not SSE). Supports both vision models (llava,
// llama3.2-vision, ...) and plain text models via `input.transcript`.
data class OllamaConfig(
    val baseUrl: String = "http://localhost:11434",
    val model: String = "llava",
)

class OllamaProvider(private val config: OllamaConfig, override val supportsVision: Boolean = true) : LlmProvider {
    private val client = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(120, TimeUnit.SECONDS)
        .build()

    override fun ask(input: TurnInput, ctx: TurnContext, remember: Boolean): Flow<OracleEvent> = callbackFlow {
        val system = systemPrompt(remember)
        val userText = turnText(ctx)
        val imageB64 = input.imagePng?.let { Base64.encodeToString(it, Base64.NO_WRAP) }
        // Text-only models have no vision path: fold the recognized transcript
        // into the instruction so they still know what was written.
        val userContent = if (imageB64 != null) {
            userText
        } else {
            input.transcript?.let { "$userText\n\nWhat was written on the page: \"$it\"" } ?: userText
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
                    put("content", userContent)
                    if (imageB64 != null) putJsonArray("images") { add(imageB64) }
                }
            }
        }.toString()

        val request = Request.Builder()
            .url("${config.baseUrl.trimEnd('/')}/api/chat")
            .post(body.toRequestBody("application/json".toMediaType()))
            .build()

        val parser = StreamParser(ctx.catalogIds)
        val acc = StringBuilder()

        val call = client.newCall(request)
        call.enqueue(object : Callback {
            override fun onFailure(call: Call, e: IOException) {
                trySend(OracleEvent.Error("request failed: ${e.message}"))
                close()
            }

            override fun onResponse(call: Call, response: Response) {
                response.use { resp ->
                    if (!resp.isSuccessful) {
                        trySend(OracleEvent.Error("http ${resp.code}: ${resp.body?.string().orEmpty().trim()}"))
                        close()
                        return
                    }
                    val source = resp.body?.source()
                    if (source == null) {
                        close()
                        return
                    }
                    while (!source.exhausted()) {
                        val line = source.readUtf8Line() ?: break
                        if (line.isBlank()) continue
                        val (frag, done) = parseOllamaLine(line) ?: continue
                        if (frag.isNotEmpty()) {
                            acc.append(frag)
                            for (e in parser.advance(acc.toString(), false)) trySend(e)
                        }
                        if (done) {
                            for (e in parser.advance(acc.toString(), true)) trySend(e)
                        }
                    }
                    close()
                }
            }
        })
        awaitClose { call.cancel() }
    }
}

private fun parseOllamaLine(line: String): Pair<String, Boolean>? = try {
    val root = Json.parseToJsonElement(line).jsonObject
    val content = root["message"]?.jsonObject?.get("content")?.jsonPrimitive?.contentOrNull ?: ""
    val done = root["done"]?.jsonPrimitive?.booleanOrNull ?: false
    content to done
} catch (e: Exception) {
    null
}

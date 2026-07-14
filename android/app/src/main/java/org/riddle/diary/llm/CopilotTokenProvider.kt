package org.riddle.diary.llm

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.IOException

// EXPERIMENTAL: exchanges the GitHub OAuth token from [CopilotDeviceFlow] for a
// short-lived (~30min) Copilot API token via the same undocumented endpoint
// community bridges use, and re-exchanges it once it's close to expiry.
class CopilotTokenProvider(private val githubOAuthToken: String) {
    private val client = OkHttpClient()
    private val mutex = Mutex()
    private var cachedToken: String? = null
    private var expiresAtEpochSeconds: Long = 0

    suspend fun token(): String = mutex.withLock {
        val now = System.currentTimeMillis() / 1000
        cachedToken?.let { tok -> if (expiresAtEpochSeconds - now > 60) return@withLock tok }
        refresh()
    }

    private suspend fun refresh(): String = withContext(Dispatchers.IO) {
        val request = Request.Builder()
            .url("https://api.github.com/copilot_internal/v2/token")
            .header("Authorization", "token $githubOAuthToken")
            .header("Accept", "application/json")
            .build()
        client.newCall(request).execute().use { resp ->
            val text = resp.body?.string().orEmpty()
            if (!resp.isSuccessful) throw IOException("copilot token exchange failed: ${resp.code}: $text")
            val json = Json.parseToJsonElement(text).jsonObject
            val tok = json.getValue("token").jsonPrimitive.content
            cachedToken = tok
            expiresAtEpochSeconds = json["expires_at"]?.jsonPrimitive?.long ?: (System.currentTimeMillis() / 1000 + 1500)
            tok
        }
    }
}

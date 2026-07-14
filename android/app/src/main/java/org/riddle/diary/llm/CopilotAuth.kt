package org.riddle.diary.llm

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import okhttp3.FormBody
import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.IOException

// EXPERIMENTAL / unofficial: GitHub's Copilot chat access has no public OAuth
// API. This mirrors the device-code flow used by community bridges
// (copilot.vim, copilot-api) to obtain a token scoped for Copilot. GitHub can
// change or revoke this at any time without notice — treat CopilotProvider as
// best-effort, isolated from the other (documented) providers.
object CopilotDeviceFlow {
    // Public client_id used by the community Copilot-chat bridges referenced
    // above. If device sign-in starts failing, this is the first thing to check
    // against an actively maintained bridge project.
    const val CLIENT_ID = "Iv1.b507a08c87ecfe98"

    data class DeviceCode(
        val deviceCode: String,
        val userCode: String,
        val verificationUri: String,
        val intervalSeconds: Int,
    )

    private val client = OkHttpClient()

    suspend fun requestDeviceCode(): DeviceCode = withContext(Dispatchers.IO) {
        val body = FormBody.Builder()
            .add("client_id", CLIENT_ID)
            .add("scope", "read:user")
            .build()
        val request = Request.Builder()
            .url("https://github.com/login/device/code")
            .header("Accept", "application/json")
            .post(body)
            .build()
        client.newCall(request).execute().use { resp ->
            val text = resp.body?.string().orEmpty()
            if (!resp.isSuccessful) throw IOException("device code request failed: ${resp.code}: $text")
            val json = Json.parseToJsonElement(text).jsonObject
            DeviceCode(
                deviceCode = json.getValue("device_code").jsonPrimitive.content,
                userCode = json.getValue("user_code").jsonPrimitive.content,
                verificationUri = json.getValue("verification_uri").jsonPrimitive.content,
                intervalSeconds = json["interval"]?.jsonPrimitive?.int ?: 5,
            )
        }
    }

    // Polls until the user authorizes at [DeviceCode.verificationUri] with
    // [DeviceCode.userCode], or the device code expires. Suspends for the
    // whole wait — call from a coroutine the caller can cancel.
    suspend fun pollForAccessToken(deviceCode: String, intervalSeconds: Int): String = withContext(Dispatchers.IO) {
        var interval = intervalSeconds
        var token: String? = null
        while (token == null) {
            delay(interval * 1000L)
            val body = FormBody.Builder()
                .add("client_id", CLIENT_ID)
                .add("device_code", deviceCode)
                .add("grant_type", "urn:ietf:params:oauth:grant-type:device_code")
                .build()
            val request = Request.Builder()
                .url("https://github.com/login/oauth/access_token")
                .header("Accept", "application/json")
                .post(body)
                .build()
            val json = client.newCall(request).execute().use { resp ->
                Json.parseToJsonElement(resp.body?.string().orEmpty()).jsonObject
            }
            token = json["access_token"]?.jsonPrimitive?.contentOrNull
            if (token == null) {
                when (json["error"]?.jsonPrimitive?.contentOrNull) {
                    "authorization_pending" -> Unit
                    "slow_down" -> interval += 5
                    "expired_token" -> throw IOException("device code expired — restart Copilot sign-in")
                    "access_denied" -> throw IOException("authorization denied")
                    else -> throw IOException("device auth failed: $json")
                }
            }
        }
        token
    }
}

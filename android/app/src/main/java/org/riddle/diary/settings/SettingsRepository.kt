package org.riddle.diary.settings

import android.content.Context
import android.content.SharedPreferences
import androidx.datastore.preferences.core.MutablePreferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import org.riddle.diary.llm.AnthropicConfig
import org.riddle.diary.llm.AnthropicProvider
import org.riddle.diary.llm.CopilotConfig
import org.riddle.diary.llm.CopilotProvider
import org.riddle.diary.llm.CopilotTokenProvider
import org.riddle.diary.llm.GeminiConfig
import org.riddle.diary.llm.GeminiProvider
import org.riddle.diary.llm.LlmProvider
import org.riddle.diary.llm.OllamaConfig
import org.riddle.diary.llm.OllamaProvider
import org.riddle.diary.llm.OpenAiCompatibleProvider
import org.riddle.diary.llm.OpenAiConfig

private val Context.dataStore by preferencesDataStore(name = "riddle_settings")

private object Keys {
    val PROVIDER = stringPreferencesKey("provider")
    val MEMORY_ENABLED = booleanPreferencesKey("memory_enabled")

    val OPENAI_BASE = stringPreferencesKey("openai_base")
    val OPENAI_MODEL = stringPreferencesKey("openai_model")
    val OPENAI_MAX_TOKENS = intPreferencesKey("openai_max_tokens")
    val OPENAI_REASONING = stringPreferencesKey("openai_reasoning")

    val OLLAMA_BASE = stringPreferencesKey("ollama_base")
    val OLLAMA_MODEL = stringPreferencesKey("ollama_model")
    val OLLAMA_VISION = booleanPreferencesKey("ollama_vision")

    val ANTHROPIC_MODEL = stringPreferencesKey("anthropic_model")
    val GEMINI_MODEL = stringPreferencesKey("gemini_model")
    val COPILOT_MODEL = stringPreferencesKey("copilot_model")
}

data class SettingsSnapshot(
    val provider: ProviderKind = ProviderKind.OPENAI_COMPATIBLE,
    val memoryEnabled: Boolean = true,
    val openaiBase: String = "https://api.openai.com/v1",
    val openaiModel: String = "gpt-4o-mini",
    val openaiMaxTokens: Int = 2000,
    val openaiReasoning: String = "",
    val ollamaBase: String = "http://localhost:11434",
    val ollamaModel: String = "llava",
    val ollamaVision: Boolean = true,
    val anthropicModel: String = "claude-3-5-sonnet-20241022",
    val geminiModel: String = "gemini-2.0-flash",
    val copilotModel: String = "gpt-4o",
)

// Non-secret config lives in DataStore; API keys and the Copilot GitHub OAuth
// token live in EncryptedSharedPreferences (android-crypto-backed). Mirrors
// the original's oracle.env / settings.schema.json, one field set per provider.
class SettingsRepository(private val context: Context) {
    private val secrets: SharedPreferences by lazy {
        val masterKey = MasterKey.Builder(context).setKeyScheme(MasterKey.KeyScheme.AES256_GCM).build()
        EncryptedSharedPreferences.create(
            context,
            "riddle_secrets",
            masterKey,
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
            EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM,
        )
    }

    var openaiApiKey: String?
        get() = secrets.getString("openai_key", null)
        set(v) = secrets.edit().putString("openai_key", v).apply()

    var anthropicApiKey: String?
        get() = secrets.getString("anthropic_key", null)
        set(v) = secrets.edit().putString("anthropic_key", v).apply()

    var geminiApiKey: String?
        get() = secrets.getString("gemini_key", null)
        set(v) = secrets.edit().putString("gemini_key", v).apply()

    var copilotGithubToken: String?
        get() = secrets.getString("copilot_gh_token", null)
        set(v) = secrets.edit().putString("copilot_gh_token", v).apply()

    val snapshot: Flow<SettingsSnapshot> = context.dataStore.data.map { p ->
        SettingsSnapshot(
            provider = p[Keys.PROVIDER]?.let { runCatching { ProviderKind.valueOf(it) }.getOrNull() }
                ?: ProviderKind.OPENAI_COMPATIBLE,
            memoryEnabled = p[Keys.MEMORY_ENABLED] ?: true,
            openaiBase = p[Keys.OPENAI_BASE] ?: "https://api.openai.com/v1",
            openaiModel = p[Keys.OPENAI_MODEL] ?: "gpt-4o-mini",
            openaiMaxTokens = p[Keys.OPENAI_MAX_TOKENS] ?: 2000,
            openaiReasoning = p[Keys.OPENAI_REASONING] ?: "",
            ollamaBase = p[Keys.OLLAMA_BASE] ?: "http://localhost:11434",
            ollamaModel = p[Keys.OLLAMA_MODEL] ?: "llava",
            ollamaVision = p[Keys.OLLAMA_VISION] ?: true,
            anthropicModel = p[Keys.ANTHROPIC_MODEL] ?: "claude-3-5-sonnet-20241022",
            geminiModel = p[Keys.GEMINI_MODEL] ?: "gemini-2.0-flash",
            copilotModel = p[Keys.COPILOT_MODEL] ?: "gpt-4o",
        )
    }

    suspend fun current(): SettingsSnapshot = snapshot.first()

    suspend fun save(update: (MutablePreferences) -> Unit) {
        context.dataStore.edit(update)
    }

    suspend fun saveSnapshot(s: SettingsSnapshot) = save { p ->
        p[Keys.PROVIDER] = s.provider.name
        p[Keys.MEMORY_ENABLED] = s.memoryEnabled
        p[Keys.OPENAI_BASE] = s.openaiBase
        p[Keys.OPENAI_MODEL] = s.openaiModel
        p[Keys.OPENAI_MAX_TOKENS] = s.openaiMaxTokens
        p[Keys.OPENAI_REASONING] = s.openaiReasoning
        p[Keys.OLLAMA_BASE] = s.ollamaBase
        p[Keys.OLLAMA_MODEL] = s.ollamaModel
        p[Keys.OLLAMA_VISION] = s.ollamaVision
        p[Keys.ANTHROPIC_MODEL] = s.anthropicModel
        p[Keys.GEMINI_MODEL] = s.geminiModel
        p[Keys.COPILOT_MODEL] = s.copilotModel
    }

    // Null when the selected provider is missing its required secret (API
    // key / Copilot sign-in) — callers show a "configure a provider" prompt.
    suspend fun buildProvider(): LlmProvider? {
        val s = current()
        return when (s.provider) {
            ProviderKind.OPENAI_COMPATIBLE -> openaiApiKey?.let { key ->
                OpenAiCompatibleProvider(
                    OpenAiConfig(
                        baseUrl = s.openaiBase,
                        apiKey = key,
                        model = s.openaiModel,
                        maxTokens = s.openaiMaxTokens,
                        reasoningEffort = s.openaiReasoning.ifBlank { null },
                    ),
                )
            }
            ProviderKind.OLLAMA -> OllamaProvider(
                OllamaConfig(baseUrl = s.ollamaBase, model = s.ollamaModel),
                supportsVision = s.ollamaVision,
            )
            ProviderKind.ANTHROPIC -> anthropicApiKey?.let { key ->
                AnthropicProvider(AnthropicConfig(apiKey = key, model = s.anthropicModel))
            }
            ProviderKind.GEMINI -> geminiApiKey?.let { key ->
                GeminiProvider(GeminiConfig(apiKey = key, model = s.geminiModel))
            }
            ProviderKind.COPILOT -> copilotGithubToken?.let { token ->
                CopilotProvider(CopilotTokenProvider(token), CopilotConfig(model = s.copilotModel))
            }
        }
    }
}

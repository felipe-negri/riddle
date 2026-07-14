package org.riddle.diary.settings

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import org.riddle.diary.llm.CopilotDeviceFlow

sealed class CopilotSignInState {
    object Idle : CopilotSignInState()
    data class AwaitingUser(val userCode: String, val verificationUri: String) : CopilotSignInState()
    object Signing : CopilotSignInState()
    object SignedIn : CopilotSignInState()
    data class Failed(val message: String) : CopilotSignInState()
}

class SettingsViewModel(application: Application) : AndroidViewModel(application) {
    private val repo = SettingsRepository(application)

    val snapshot: StateFlow<SettingsSnapshot> = run {
        val flow = MutableStateFlow(SettingsSnapshot())
        viewModelScope.launch { repo.snapshot.collect { flow.value = it } }
        flow.asStateFlow()
    }

    private val _copilotState = MutableStateFlow<CopilotSignInState>(
        if (repo.copilotGithubToken != null) CopilotSignInState.SignedIn else CopilotSignInState.Idle,
    )
    val copilotState: StateFlow<CopilotSignInState> = _copilotState.asStateFlow()
    private var copilotJob: Job? = null

    fun save(update: SettingsSnapshot.() -> SettingsSnapshot) {
        viewModelScope.launch { repo.saveSnapshot(snapshot.value.update()) }
    }

    fun setOpenAiApiKey(key: String) {
        repo.openaiApiKey = key
    }

    fun setAnthropicApiKey(key: String) {
        repo.anthropicApiKey = key
    }

    fun setGeminiApiKey(key: String) {
        repo.geminiApiKey = key
    }

    fun openAiApiKey(): String = repo.openaiApiKey.orEmpty()
    fun anthropicApiKey(): String = repo.anthropicApiKey.orEmpty()
    fun geminiApiKey(): String = repo.geminiApiKey.orEmpty()

    fun startCopilotSignIn() {
        copilotJob?.cancel()
        _copilotState.value = CopilotSignInState.Signing
        copilotJob = viewModelScope.launch {
            try {
                val device = CopilotDeviceFlow.requestDeviceCode()
                _copilotState.value = CopilotSignInState.AwaitingUser(device.userCode, device.verificationUri)
                val token = CopilotDeviceFlow.pollForAccessToken(device.deviceCode, device.intervalSeconds)
                repo.copilotGithubToken = token
                _copilotState.value = CopilotSignInState.SignedIn
            } catch (e: Exception) {
                _copilotState.value = CopilotSignInState.Failed(e.message ?: "sign-in failed")
            }
        }
    }
}

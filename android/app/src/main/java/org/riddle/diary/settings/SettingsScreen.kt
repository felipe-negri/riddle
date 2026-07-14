package org.riddle.diary.settings

import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(onDone: () -> Unit, viewModel: SettingsViewModel = viewModel()) {
    val s by viewModel.snapshot.collectAsState()
    val copilotState by viewModel.copilotState.collectAsState()
    val context = LocalContext.current

    Scaffold(topBar = { TopAppBar(title = { Text("Settings") }) }) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text("Provider", style = MaterialTheme.typography.titleMedium)
            ProviderKind.entries.forEach { kind ->
                Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                    RadioButton(selected = s.provider == kind, onClick = { viewModel.save { copy(provider = kind) } })
                    Text(kind.label)
                }
            }

            HorizontalDivider()

            when (s.provider) {
                ProviderKind.OPENAI_COMPATIBLE -> OpenAiFields(s, viewModel)
                ProviderKind.OLLAMA -> OllamaFields(s, viewModel)
                ProviderKind.ANTHROPIC -> AnthropicFields(s, viewModel)
                ProviderKind.GEMINI -> GeminiFields(s, viewModel)
                ProviderKind.COPILOT -> CopilotFields(s, viewModel, copilotState, onOpenBrowser = { uri ->
                    context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(uri)))
                })
            }

            HorizontalDivider()

            Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                Text("The diary remembers", modifier = Modifier.padding(end = 8.dp))
                Switch(checked = s.memoryEnabled, onCheckedChange = { viewModel.save { copy(memoryEnabled = it) } })
            }

            Button(onClick = onDone, modifier = Modifier.fillMaxWidth()) { Text("Done") }
        }
    }
}

@Composable
private fun OpenAiFields(s: SettingsSnapshot, vm: SettingsViewModel) {
    var apiKey by remember { mutableStateOf(vm.openAiApiKey()) }
    OutlinedTextField(s.openaiBase, { vm.save { copy(openaiBase = it) } }, label = { Text("Base URL") }, modifier = Modifier.fillMaxWidth())
    OutlinedTextField(s.openaiModel, { vm.save { copy(openaiModel = it) } }, label = { Text("Model") }, modifier = Modifier.fillMaxWidth())
    OutlinedTextField(
        apiKey,
        { apiKey = it; vm.setOpenAiApiKey(it) },
        label = { Text("API key") },
        modifier = Modifier.fillMaxWidth(),
    )
    OutlinedTextField(
        s.openaiMaxTokens.toString(),
        { it.toIntOrNull()?.let { n -> vm.save { copy(openaiMaxTokens = n) } } },
        label = { Text("Max tokens") },
        modifier = Modifier.fillMaxWidth(),
    )
    OutlinedTextField(
        s.openaiReasoning,
        { vm.save { copy(openaiReasoning = it) } },
        label = { Text("Reasoning effort (low/medium/high, optional)") },
        modifier = Modifier.fillMaxWidth(),
    )
}

@Composable
private fun OllamaFields(s: SettingsSnapshot, vm: SettingsViewModel) {
    OutlinedTextField(s.ollamaBase, { vm.save { copy(ollamaBase = it) } }, label = { Text("Server URL") }, modifier = Modifier.fillMaxWidth())
    OutlinedTextField(s.ollamaModel, { vm.save { copy(ollamaModel = it) } }, label = { Text("Model") }, modifier = Modifier.fillMaxWidth())
    Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
        Text("Model supports vision", modifier = Modifier.padding(end = 8.dp))
        Switch(checked = s.ollamaVision, onCheckedChange = { vm.save { copy(ollamaVision = it) } })
    }
}

@Composable
private fun AnthropicFields(s: SettingsSnapshot, vm: SettingsViewModel) {
    var apiKey by remember { mutableStateOf(vm.anthropicApiKey()) }
    OutlinedTextField(s.anthropicModel, { vm.save { copy(anthropicModel = it) } }, label = { Text("Model") }, modifier = Modifier.fillMaxWidth())
    OutlinedTextField(apiKey, { apiKey = it; vm.setAnthropicApiKey(it) }, label = { Text("API key") }, modifier = Modifier.fillMaxWidth())
}

@Composable
private fun GeminiFields(s: SettingsSnapshot, vm: SettingsViewModel) {
    var apiKey by remember { mutableStateOf(vm.geminiApiKey()) }
    OutlinedTextField(s.geminiModel, { vm.save { copy(geminiModel = it) } }, label = { Text("Model") }, modifier = Modifier.fillMaxWidth())
    OutlinedTextField(apiKey, { apiKey = it; vm.setGeminiApiKey(it) }, label = { Text("API key") }, modifier = Modifier.fillMaxWidth())
}

@Composable
private fun CopilotFields(
    s: SettingsSnapshot,
    vm: SettingsViewModel,
    state: CopilotSignInState,
    onOpenBrowser: (String) -> Unit,
) {
    Text(
        "Experimental: uses GitHub's undocumented Copilot chat API. May stop working without notice.",
        style = MaterialTheme.typography.bodySmall,
    )
    OutlinedTextField(s.copilotModel, { vm.save { copy(copilotModel = it) } }, label = { Text("Model") }, modifier = Modifier.fillMaxWidth())
    when (state) {
        is CopilotSignInState.Idle -> Button(onClick = vm::startCopilotSignIn) { Text("Sign in with GitHub") }
        is CopilotSignInState.Signing -> Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            CircularProgressIndicator(modifier = Modifier.padding(end = 8.dp))
            Text("Starting sign-in…")
        }
        is CopilotSignInState.AwaitingUser -> Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Enter this code at ${state.verificationUri}:")
            Text(state.userCode, style = MaterialTheme.typography.headlineMedium)
            Button(onClick = { onOpenBrowser(state.verificationUri) }) { Text("Open GitHub") }
        }
        is CopilotSignInState.SignedIn -> Text("Signed in.")
        is CopilotSignInState.Failed -> Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Sign-in failed: ${state.message}")
            Button(onClick = vm::startCopilotSignIn) { Text("Retry") }
        }
    }
}

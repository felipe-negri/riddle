package org.riddle.diary.ui

import android.app.Application
import android.graphics.Bitmap
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import org.riddle.diary.input.Stroke
import org.riddle.diary.llm.OracleEvent
import org.riddle.diary.llm.TurnContext
import org.riddle.diary.llm.TurnInput
import org.riddle.diary.memory.MemoryStore
import org.riddle.diary.memory.RiddleDatabase
import org.riddle.diary.recognition.InkRecognizer
import org.riddle.diary.settings.SettingsRepository
import org.riddle.diary.util.toPngBytes

private const val CATALOG_MAX = 20
private const val HISTORY_TURNS = 6

data class DiaryUiState(
    val thinking: Boolean = false,
    val replyText: String = "",
    val error: String? = null,
)

class DiaryViewModel(application: Application) : AndroidViewModel(application) {
    private val _state = MutableStateFlow(DiaryUiState())
    val state: StateFlow<DiaryUiState> = _state.asStateFlow()

    private var askJob: Job? = null
    private val recognizer = InkRecognizer()
    private val memory = MemoryStore(RiddleDatabase.get(application).memoryDao())
    private val settings = SettingsRepository(application)

    fun onPageCommitted(strokes: List<Stroke>, bitmap: Bitmap) {
        askJob?.cancel()
        _state.value = _state.value.copy(thinking = true, replyText = "", error = null)
        val pageId = System.currentTimeMillis() / 1000
        val png = bitmap.toPngBytes()
        askJob = viewModelScope.launch {
            // Settings are read fresh each turn, so switching provider/model in
            // SettingsScreen takes effect on the very next committed page.
            val p = settings.buildProvider() ?: run {
                _state.value = _state.value.copy(thinking = false, error = "configure a provider in Settings first")
                return@launch
            }
            val memoryOn = settings.current().memoryEnabled

            // On-device handwriting -> text: fed to text-only providers (Ollama
            // text models, Copilot) and stored as the page's memory transcript.
            val recognized = runCatching { recognizer.recognize(strokes) }.getOrNull()
            val input = TurnInput(transcript = recognized, imagePng = if (p.supportsVision) png else null)
            val ctx = if (memoryOn) {
                val (catalogLines, catalogIds) = memory.catalog(CATALOG_MAX)
                TurnContext(history = memory.recentDialogue(HISTORY_TURNS), catalogLines = catalogLines, catalogIds = catalogIds)
            } else {
                TurnContext()
            }

            var reply = ""
            var transcript = recognized.orEmpty()
            p.ask(input, ctx, remember = memoryOn).collect { event ->
                when (event) {
                    is OracleEvent.Ink -> {
                        reply = (reply + " " + event.text).trim()
                        _state.value = _state.value.copy(replyText = reply, thinking = false)
                    }
                    // The model's own faithful transcription (⁂) beats the
                    // on-device recognizer when both are available.
                    is OracleEvent.Transcript -> transcript = event.text
                    is OracleEvent.Show -> {
                        val recalled = memory.get(event.memoryId)
                        if (recalled != null) {
                            reply = recalled.reply
                            _state.value = _state.value.copy(replyText = reply, thinking = false)
                        }
                    }
                    is OracleEvent.Error -> _state.value = _state.value.copy(error = event.message, thinking = false)
                }
            }
            _state.value = _state.value.copy(thinking = false)
            if (memoryOn && reply.isNotBlank()) {
                memory.append(pageId, transcript, reply, strokes)
            }
        }
    }

    override fun onCleared() {
        recognizer.close()
    }
}

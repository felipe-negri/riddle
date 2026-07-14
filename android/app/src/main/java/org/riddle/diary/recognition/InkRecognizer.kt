package org.riddle.diary.recognition

import com.google.mlkit.common.model.DownloadConditions
import com.google.mlkit.common.model.RemoteModelManager
import com.google.mlkit.vision.digitalink.DigitalInkRecognition
import com.google.mlkit.vision.digitalink.DigitalInkRecognitionModel
import com.google.mlkit.vision.digitalink.DigitalInkRecognitionModelIdentifier
import com.google.mlkit.vision.digitalink.DigitalInkRecognizerOptions
import com.google.mlkit.vision.digitalink.Ink
import kotlinx.coroutines.tasks.await
import org.riddle.diary.input.Stroke

// Strokes -> text, entirely on-device (no network, no vendor API). Feeds the
// LLM providers that don't understand images (task text-only Ollama models,
// Copilot) and drives the ⁂-transcript recall/memory catalog either way.
class InkRecognizer(languageTag: String = "en-US") {
    private val modelIdentifier: DigitalInkRecognitionModelIdentifier =
        DigitalInkRecognitionModelIdentifier.fromLanguageTag(languageTag)
            ?: DigitalInkRecognitionModelIdentifier.fromLanguageTag("en-US")!!
    private val model: DigitalInkRecognitionModel = DigitalInkRecognitionModel.builder(modelIdentifier).build()
    private val remoteModelManager = RemoteModelManager.getInstance()
    private val recognizer by lazy {
        DigitalInkRecognition.getClient(DigitalInkRecognizerOptions.builder(model).build())
    }

    suspend fun ensureModelDownloaded() {
        val downloaded = remoteModelManager.isModelDownloaded(model).await()
        if (!downloaded) {
            remoteModelManager.download(model, DownloadConditions.Builder().build()).await()
        }
    }

    suspend fun recognize(strokes: List<Stroke>): String? {
        if (strokes.isEmpty()) return null
        ensureModelDownloaded()
        val builder = Ink.builder()
        for (stroke in strokes) {
            val strokeBuilder = Ink.Stroke.builder()
            for (p in stroke.points) {
                strokeBuilder.addPoint(Ink.Point.create(p.x, p.y, p.tMillis))
            }
            builder.addStroke(strokeBuilder.build())
        }
        val candidates = recognizer.recognize(builder.build()).await().candidates
        return candidates.firstOrNull()?.text?.takeIf { it.isNotBlank() }
    }

    fun close() = recognizer.close()
}

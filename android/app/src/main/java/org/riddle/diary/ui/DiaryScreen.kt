package org.riddle.diary.ui

import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.Row
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.HelpOutline
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import org.riddle.diary.ink.HandwritingReply
import org.riddle.diary.input.StylusCanvas

@Composable
fun DiaryScreen(onOpenSettings: () -> Unit, viewModel: DiaryViewModel = viewModel()) {
    val state by viewModel.state.collectAsState()
    var showHelp by remember { mutableStateOf(false) }

    BoxWithConstraints(modifier = Modifier.fillMaxSize()) {
        val replyWidth = maxWidth - 32.dp

        StylusCanvas(
            modifier = Modifier.fillMaxSize(),
            onCommit = { strokes, bitmap -> viewModel.onPageCommitted(strokes, bitmap) },
        )

        Row(modifier = Modifier.align(Alignment.TopStart).padding(8.dp)) {
            IconButton(onClick = onOpenSettings) {
                Icon(Icons.Default.Settings, contentDescription = "Settings")
            }
            IconButton(onClick = { showHelp = true }) {
                Icon(Icons.Default.HelpOutline, contentDescription = "Help")
            }
        }

        if (showHelp) {
            HelpOverlay(onDismiss = { showHelp = false })
        }

        if (state.thinking) {
            CircularProgressIndicator(modifier = Modifier.align(Alignment.TopEnd).padding(16.dp))
        }

        if (state.replyText.isNotEmpty()) {
            HandwritingReply(
                text = state.replyText,
                width = replyWidth,
                modifier = Modifier.align(Alignment.BottomCenter).padding(16.dp),
            )
        }

        state.error?.let { err ->
            Card(modifier = Modifier.align(Alignment.BottomCenter).fillMaxWidth().padding(16.dp)) {
                Text("error: $err", modifier = Modifier.padding(16.dp))
            }
        }
    }
}

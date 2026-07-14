package org.riddle.diary.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp

// Ported from the original's help.rs guide panel text (its "draw a big ? to
// summon this" trick doesn't translate to a touchscreen with real buttons —
// a help icon on DiaryScreen opens this directly instead).
private val LINES = listOf(
    "Write, then rest your quill:",
    "the diary drinks your ink and Tom replies.",
    "",
    "The diary remembers. Ask it:",
    "\"show me what I wrote about...\"",
    "and the page will rise again.",
    "",
    "Use the S-Pen's eraser tip to erase.",
    "Tap the gear icon for settings.",
)

@Composable
fun HelpOverlay(onDismiss: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.White.copy(alpha = 0.96f))
            .clickable(onClick = onDismiss)
            .padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text("The Diary", style = MaterialTheme.typography.headlineMedium)
        Spacer(modifier = Modifier.padding(12.dp))
        LINES.forEach { line ->
            Text(line.ifEmpty { " " }, textAlign = TextAlign.Center, style = MaterialTheme.typography.bodyLarge)
        }
        Spacer(modifier = Modifier.padding(24.dp))
        Text("Tap anywhere to close.", style = MaterialTheme.typography.bodySmall)
    }
}

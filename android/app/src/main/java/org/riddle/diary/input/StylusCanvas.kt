package org.riddle.diary.input

import android.graphics.Bitmap
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView

// Fires onCommit 2.8s after the last stroke with the page's strokes + bitmap.
@Composable
fun StylusCanvas(
    modifier: Modifier = Modifier,
    onInkStarted: () -> Unit = {},
    onCommit: (List<Stroke>, Bitmap) -> Unit,
) {
    AndroidView(
        modifier = modifier.fillMaxSize(),
        factory = { context ->
            StylusCanvasView(context).apply {
                this.onCommit = onCommit
                this.onInkStarted = onInkStarted
            }
        },
        update = { view ->
            view.onCommit = onCommit
            view.onInkStarted = onInkStarted
        },
    )
}

package org.riddle.diary.util

import android.graphics.Bitmap
import java.io.ByteArrayOutputStream

fun Bitmap.toPngBytes(): ByteArray = ByteArrayOutputStream().use { out ->
    compress(Bitmap.CompressFormat.PNG, 100, out)
    out.toByteArray()
}

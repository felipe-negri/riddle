package org.riddle.diary.ui

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import org.riddle.diary.settings.SettingsScreen

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            MaterialTheme {
                Scaffold { padding ->
                    Surface(modifier = Modifier.fillMaxSize().padding(padding)) {
                        var showSettings by remember { mutableStateOf(false) }
                        if (showSettings) {
                            SettingsScreen(onDone = { showSettings = false })
                        } else {
                            DiaryScreen(onOpenSettings = { showSettings = true })
                        }
                    }
                }
            }
        }
    }
}

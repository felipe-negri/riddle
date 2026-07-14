package org.riddle.diary.memory

import androidx.room.Entity
import androidx.room.PrimaryKey

@Entity(tableName = "memories")
data class MemoryEntity(
    // Unix seconds when the page was committed — also the catalog sort key.
    @PrimaryKey val id: Long,
    val transcript: String,
    val reply: String,
    // Decimated pen strokes as JSON ([[ {x,y,r}, ... ], ...]) — kept for a
    // future "replay the page in the writer's hand" recall view.
    val strokesJson: String,
)

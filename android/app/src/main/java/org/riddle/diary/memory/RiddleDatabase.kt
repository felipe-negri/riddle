package org.riddle.diary.memory

import android.content.Context
import androidx.room.Database
import androidx.room.Room
import androidx.room.RoomDatabase

@Database(entities = [MemoryEntity::class], version = 1, exportSchema = false)
abstract class RiddleDatabase : RoomDatabase() {
    abstract fun memoryDao(): MemoryDao

    companion object {
        @Volatile private var instance: RiddleDatabase? = null

        fun get(context: Context): RiddleDatabase = instance ?: synchronized(this) {
            instance ?: Room.databaseBuilder(context.applicationContext, RiddleDatabase::class.java, "riddle-memory.db")
                .build()
                .also { instance = it }
        }
    }
}

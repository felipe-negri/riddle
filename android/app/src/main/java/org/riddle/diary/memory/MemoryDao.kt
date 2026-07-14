package org.riddle.diary.memory

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query

@Dao
interface MemoryDao {
    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun insert(entry: MemoryEntity)

    // Oldest first — callers reverse when they need newest-first.
    @Query("SELECT * FROM memories ORDER BY id ASC")
    suspend fun all(): List<MemoryEntity>

    @Query("SELECT * FROM memories WHERE id = :id")
    suspend fun get(id: Long): MemoryEntity?

    @Query("SELECT COUNT(*) FROM memories")
    suspend fun count(): Int

    @Query("DELETE FROM memories WHERE id IN (SELECT id FROM memories ORDER BY id ASC LIMIT :n)")
    suspend fun deleteOldest(n: Int)
}

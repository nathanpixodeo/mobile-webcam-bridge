package dev.mobilewebcambridge.protocol.logging

import dev.mobilewebcambridge.protocol.messages.LogLevel
import dev.mobilewebcambridge.protocol.messages.LogMessage

/** One log record kept for forwarding to the host and for the diagnostics screen. */
data class LogEntry(
    val timestampUs: Long,
    val level: LogLevel,
    val category: String,
    val message: String,
) {
    val logMessage: LogMessage get() = LogMessage(level, category, message)
}

/**
 * Bounded log buffer: when full, the oldest entry is discarded and counted. Not thread-safe;
 * callers synchronise.
 */
class LogRing(capacity: Int = 1_000) {
    val capacity: Int = capacity.coerceAtLeast(1)
    private val entries = ArrayDeque<LogEntry>()

    var droppedCount: Int = 0
        private set

    val count: Int get() = entries.size
    val isEmpty: Boolean get() = entries.isEmpty()

    fun append(entry: LogEntry) {
        entries.addLast(entry)
        while (entries.size > capacity) {
            entries.removeFirst()
            droppedCount++
        }
    }

    /** Removes and returns up to [maxCount] of the oldest entries. */
    fun drain(maxCount: Int): List<LogEntry> {
        val drained = ArrayList<LogEntry>(minOf(maxCount, entries.size))
        while (drained.size < maxCount) drained += entries.removeFirstOrNull() ?: break
        return drained
    }

    /** The newest [count] entries, oldest first, without removing them. */
    fun newest(count: Int): List<LogEntry> = entries.takeLast(count.coerceAtLeast(0))

    /** Returns the number of entries dropped since the last call and resets the counter. */
    fun takeDroppedCount(): Int {
        val dropped = droppedCount
        droppedCount = 0
        return dropped
    }
}

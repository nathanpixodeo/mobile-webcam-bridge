package dev.mobilewebcambridge.android.core

import android.util.Log
import dev.mobilewebcambridge.protocol.logging.LogEntry
import dev.mobilewebcambridge.protocol.logging.LogRing
import dev.mobilewebcambridge.protocol.messages.LogLevel
import dev.mobilewebcambridge.protocol.time.MonotonicClock
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Logs to logcat, keeps a tail for the diagnostics screen, and queues info-and-above entries for
 * forwarding to the host (there is no debugger in the field, so the host log is the app's log).
 * Thread-safe.
 */
class AppLogger(private val clock: MonotonicClock) {
    /** Entries taken for forwarding, plus how many were lost to the ring's bound. */
    data class Batch(val entries: List<LogEntry>, val dropped: Int)

    private val lock = Any()
    private val forwarding = LogRing(capacity = 1_000)
    private val tail = ArrayDeque<LogEntry>()
    private val recentEntries = MutableStateFlow<List<LogEntry>>(emptyList())

    /** The newest entries, oldest first, for the diagnostics screen. */
    val recent: StateFlow<List<LogEntry>> = recentEntries.asStateFlow()

    fun debug(category: String, message: String) = log(LogLevel.DEBUG, category, message)

    fun info(category: String, message: String) = log(LogLevel.INFO, category, message)

    fun warn(category: String, message: String) = log(LogLevel.WARN, category, message)

    fun error(category: String, message: String) = log(LogLevel.ERROR, category, message)

    fun log(level: LogLevel, category: String, message: String) {
        val entry = LogEntry(clock.nowMicros(), level, category, message)
        Log.println(priority(level), TAG, "[$category] $message")
        synchronized(lock) {
            if (level >= LogLevel.INFO) forwarding.append(entry)
            tail.addLast(entry)
            while (tail.size > TAIL_SIZE) tail.removeFirst()
            recentEntries.value = tail.toList()
        }
    }

    /** Removes up to [maxCount] entries queued for the host. */
    fun takeForwardingBatch(maxCount: Int): Batch =
        synchronized(lock) { Batch(forwarding.drain(maxCount), forwarding.takeDroppedCount()) }

    private fun priority(level: LogLevel): Int = when (level) {
        LogLevel.DEBUG -> Log.DEBUG
        LogLevel.INFO -> Log.INFO
        LogLevel.WARN -> Log.WARN
        LogLevel.ERROR -> Log.ERROR
    }

    private companion object {
        /** One tag for the whole app: `adb logcat -s MobileWebcam`. */
        const val TAG = "MobileWebcam"
        const val TAIL_SIZE = 200
    }
}

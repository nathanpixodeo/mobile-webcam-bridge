package dev.mobilewebcambridge.android.core

import android.os.SystemClock
import dev.mobilewebcambridge.protocol.time.MonotonicClock

/**
 * The device clock of the wire protocol: `elapsedRealtime`, which keeps counting during deep
 * sleep and is the time base of camera sensor timestamps (`SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME`)
 * and of `AudioTimestamp.TIMEBASE_BOOTTIME`.
 */
object ElapsedRealtimeClock : MonotonicClock {
    override fun nowMicros(): Long = SystemClock.elapsedRealtimeNanos() / 1_000
}

package dev.mobilewebcambridge.protocol.session

/**
 * Liveness rules of SPEC §2.4: ping every second once handshaken, give up after 4 s without any
 * inbound packet. Not thread-safe; confine to the session thread.
 */
class HeartbeatWatchdog(
    val pingIntervalUs: Long = 1_000_000,
    val timeoutUs: Long = 4_000_000,
) {
    enum class Action { SEND_PING, TIMED_OUT }

    private var lastInboundUs: Long? = null
    private var lastPingUs: Long? = null

    val isRunning: Boolean get() = lastInboundUs != null

    fun start(nowUs: Long) {
        lastInboundUs = nowUs
        lastPingUs = null
    }

    fun stop() {
        lastInboundUs = null
        lastPingUs = null
    }

    fun noteInbound(nowUs: Long) {
        if (isRunning) lastInboundUs = nowUs
    }

    /** Call periodically (every ~100 ms). */
    fun tick(nowUs: Long): Action? {
        val lastInbound = lastInboundUs ?: return null
        if (nowUs - lastInbound >= timeoutUs) {
            stop()
            return Action.TIMED_OUT
        }
        val lastPing = lastPingUs
        if (lastPing != null && nowUs - lastPing < pingIntervalUs) return null
        lastPingUs = nowUs
        return Action.SEND_PING
    }
}

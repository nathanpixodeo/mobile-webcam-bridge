package dev.mobilewebcambridge.android.transport

import dev.mobilewebcambridge.android.core.AppLogger
import java.io.IOException
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket

/**
 * Listens on `127.0.0.1:27100` (SPEC §1). The host reaches it through ADB: the ADB server opens
 * `host:transport:<serial>` and then `tcp:27100`, and adbd connects to this port on the phone's
 * loopback. Nothing listens on a network interface.
 *
 * Accepted sockets are handed to [onAccept]; the session keeps one connection at a time (a new one
 * replaces the old). If the port is busy, binding is retried with backoff.
 */
class CompanionServer(
    private val port: Int,
    private val logger: AppLogger,
    private val onState: (State) -> Unit,
    private val onAccept: (Socket) -> Unit,
) {
    sealed interface State {
        data object Starting : State

        data class Listening(val port: Int) : State

        data class Failed(val message: String) : State

        data object Stopped : State
    }

    @Volatile
    private var running = false

    @Volatile
    private var serverSocket: ServerSocket? = null
    private var acceptThread: Thread? = null

    /** Main thread. */
    fun start() {
        if (running) return
        running = true
        onState(State.Starting)
        acceptThread = Thread({ acceptLoop() }, "mwb-server").apply { start() }
    }

    /** Main thread. */
    fun stop() {
        if (!running) return
        running = false
        runCatching { serverSocket?.close() } // unblocks accept()
        acceptThread?.interrupt() // ends a backoff sleep
        acceptThread?.join(JOIN_TIMEOUT_MS)
        acceptThread = null
        onState(State.Stopped)
    }

    private fun acceptLoop() {
        var backoffMs = INITIAL_BACKOFF_MS
        while (running) {
            val server = try {
                ServerSocket().apply {
                    reuseAddress = true
                    bind(InetSocketAddress(LOOPBACK, port), BACKLOG)
                }
            } catch (error: IOException) {
                logger.error("transport", "Cannot listen on 127.0.0.1:$port: ${error.message}")
                onState(State.Failed("Cannot listen on port $port: ${error.message}"))
                try {
                    Thread.sleep(backoffMs)
                } catch (interrupted: InterruptedException) {
                    return // stop()
                }
                backoffMs = (backoffMs * 2).coerceAtMost(MAX_BACKOFF_MS)
                continue
            }
            serverSocket = server
            if (!running) { // stop() ran before serverSocket was visible to it
                runCatching { server.close() }
                return
            }
            backoffMs = INITIAL_BACKOFF_MS
            logger.info("transport", "Listening on 127.0.0.1:$port")
            onState(State.Listening(port))
            try {
                while (running) onAccept(server.accept())
            } catch (error: IOException) {
                if (running) {
                    logger.warn("transport", "Listener failed: ${error.message}; restarting it")
                    onState(State.Failed("Listener failed: ${error.message}"))
                }
            } finally {
                runCatching { server.close() }
                serverSocket = null
            }
        }
    }

    private companion object {
        val LOOPBACK: InetAddress = InetAddress.getByAddress(byteArrayOf(127, 0, 0, 1))
        const val BACKLOG = 2
        const val INITIAL_BACKOFF_MS = 500L
        const val MAX_BACKOFF_MS = 5_000L
        const val JOIN_TIMEOUT_MS = 1_000L
    }
}

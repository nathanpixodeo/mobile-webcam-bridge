package dev.mobilewebcambridge.android.transport

import dev.mobilewebcambridge.android.core.SerialThread
import dev.mobilewebcambridge.protocol.session.ConnectionId
import java.io.IOException
import java.net.Socket
import java.util.concurrent.CountDownLatch
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/** Receives the events of one host connection. Every call happens on the session thread. */
interface ConnectionListener {
    fun connectionOpened(id: ConnectionId)

    fun connectionReceived(id: ConnectionId, bytes: ByteArray)

    /** [byteCount] bytes were written to the socket: the send window is measured against this. */
    fun connectionSent(id: ConnectionId, byteCount: Int)

    /** Called exactly once per connection. */
    fun connectionClosed(id: ConnectionId, reason: String)
}

/**
 * Owns one accepted socket with a blocking reader thread and a writer thread fed by a queue, and
 * reports everything to [listener] on [events] (the session thread), so the session needs no locks.
 */
class ConnectionDriver(
    val id: ConnectionId,
    private val socket: Socket,
    private val events: SerialThread,
    private val listener: ConnectionListener,
) {
    private sealed interface Outbound {
        class Data(val bytes: ByteArray) : Outbound

        /** Graceful close: everything queued before it is still written. */
        class Close(val reason: String) : Outbound

        data object Wake : Outbound
    }

    private val outbound = LinkedBlockingQueue<Outbound>()
    private val finished = AtomicBoolean(false)
    private val readerExited = CountDownLatch(1)
    private val reader = Thread({ readLoop() }, "mwb-conn$id-in")
    private val writer = Thread({ writeLoop() }, "mwb-conn$id-out")

    @Volatile
    private var closeReason: String? = null

    val remoteDescription: String = socket.remoteSocketAddress?.toString() ?: "unknown"

    fun start() {
        runCatching { socket.tcpNoDelay = true }
        reader.start()
        writer.start()
        events.post { listener.connectionOpened(id) }
    }

    /** Queues bytes for writing. Any thread. */
    fun send(bytes: ByteArray) {
        if (!finished.get()) outbound.put(Outbound.Data(bytes))
    }

    /** Writes what is already queued, then closes. Any thread. */
    fun close(reason: String) {
        if (!finished.get()) outbound.put(Outbound.Close(reason))
    }

    private fun readLoop() {
        val buffer = ByteArray(READ_BUFFER_BYTES)
        try {
            val input = socket.getInputStream()
            while (true) {
                val count = input.read(buffer)
                if (count < 0) {
                    finish(closeReason ?: "closed by the host")
                    return
                }
                // While closing, incoming data is read and discarded so the close stays graceful.
                if (count > 0 && closeReason == null) {
                    val bytes = buffer.copyOf(count)
                    events.post { listener.connectionReceived(id, bytes) }
                }
            }
        } catch (error: IOException) {
            finish(closeReason ?: "read failed: ${error.message}")
        } finally {
            readerExited.countDown()
        }
    }

    private fun writeLoop() {
        try {
            val output = socket.getOutputStream()
            while (true) {
                when (val item = outbound.take()) {
                    is Outbound.Data -> {
                        output.write(item.bytes)
                        val count = item.bytes.size
                        events.post { listener.connectionSent(id, count) }
                    }
                    is Outbound.Close -> {
                        closeReason = item.reason
                        output.flush()
                        // FIN after the queued data; give the host a moment to read it and close
                        // its side, since closing with unread input would reset the connection.
                        runCatching { socket.shutdownOutput() }
                        readerExited.await(LINGER_MS, TimeUnit.MILLISECONDS)
                        finish(item.reason)
                        return
                    }
                    Outbound.Wake -> return
                }
            }
        } catch (error: IOException) {
            finish("write failed: ${error.message}")
        } catch (interrupted: InterruptedException) {
            finish("writer interrupted")
        }
    }

    private fun finish(reason: String) {
        if (!finished.compareAndSet(false, true)) return
        runCatching { socket.close() } // unblocks the reader
        outbound.clear()
        outbound.offer(Outbound.Wake) // unblocks the writer
        events.post { listener.connectionClosed(id, reason) }
    }

    private companion object {
        const val READ_BUFFER_BYTES = 64 * 1024
        const val LINGER_MS = 1_000L
    }
}

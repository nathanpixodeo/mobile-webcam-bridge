package dev.mobilewebcambridge.protocol.transport

import dev.mobilewebcambridge.protocol.media.AudioChunk
import dev.mobilewebcambridge.protocol.wire.Packet
import dev.mobilewebcambridge.protocol.wire.PacketCodec

/** What kind of traffic a packet is; decides its priority and drop policy. */
sealed interface OutboundClass {
    data object Control : OutboundClass

    data object Audio : OutboundClass

    data class Video(val isKeyframe: Boolean, val isDisposable: Boolean) : OutboundClass

    data object Log : OutboundClass
}

/** A packet waiting to be written, with its wire bytes computed once. */
class OutboundItem(packet: Packet, val kind: OutboundClass) {
    val bytes: ByteArray = PacketCodec.encode(packet)

    internal val isVideoKeyframe: Boolean get() = (kind as? OutboundClass.Video)?.isKeyframe == true

    internal val isDisposableVideo: Boolean
        get() = (kind as? OutboundClass.Video)?.let { it.isDisposable && !it.isKeyframe } == true
}

/**
 * Orders outgoing traffic and protects latency under congestion (SPEC §3.4, mirrors BridgeKit).
 *
 * - Priority: control > audio > video > log.
 * - At most [Configuration.windowBytes] are handed to the connection and not yet confirmed.
 * - Video is the only traffic that is shed: disposable frames first, then everything up to the next
 *   IDR, at which point a keyframe is requested.
 * - Audio is never dropped; more than one second of queued audio is reported as fatal.
 * - Logs are bounded; the oldest are dropped.
 *
 * Not thread-safe; confine to the session thread.
 */
class SendScheduler(val configuration: Configuration = Configuration()) {
    data class Configuration(
        val windowBytes: Int = 256 * 1024,
        val maxQueuedVideoFrames: Int = 3,
        val maxQueuedAudioBytes: Int = AudioChunk.SAMPLE_RATE * AudioChunk.BYTES_PER_SAMPLE,
        val maxQueuedLogs: Int = 200,
    )

    data class EnqueueResult(
        val droppedVideoFrames: Int = 0,
        val droppedLogs: Int = 0,
        /** The encoder must produce an IDR: frames were dropped and decoding can only resume there. */
        val keyframeNeeded: Boolean = false,
        /** More audio is queued than the connection can ever catch up with; close it. */
        val audioBacklogExceeded: Boolean = false,
    )

    var inFlightBytes: Int = 0
        private set
    var queuedAudioBytes: Int = 0
        private set

    private val control = ArrayDeque<OutboundItem>()
    private val audio = ArrayDeque<OutboundItem>()
    private val video = ArrayDeque<OutboundItem>()
    private val logs = ArrayDeque<OutboundItem>()
    private var droppingUntilKeyframe = false

    val queuedVideoFrames: Int get() = video.size
    val queuedLogCount: Int get() = logs.size
    val isIdle: Boolean get() = control.isEmpty() && audio.isEmpty() && video.isEmpty() && logs.isEmpty()

    fun enqueue(item: OutboundItem): EnqueueResult =
        when (item.kind) {
            OutboundClass.Control -> {
                control.addLast(item)
                EnqueueResult()
            }
            OutboundClass.Audio -> {
                audio.addLast(item)
                queuedAudioBytes += item.bytes.size
                EnqueueResult(audioBacklogExceeded = queuedAudioBytes > configuration.maxQueuedAudioBytes)
            }
            is OutboundClass.Video -> enqueueVideo(item)
            OutboundClass.Log -> {
                logs.addLast(item)
                var dropped = 0
                while (logs.size > configuration.maxQueuedLogs) {
                    logs.removeFirst()
                    dropped++
                }
                EnqueueResult(droppedLogs = dropped)
            }
        }

    /**
     * Next item to write, or null when nothing is queued or the window is full. A single item
     * larger than the window is still sent once nothing else is in flight.
     */
    fun dequeue(): OutboundItem? {
        val next = peekNext() ?: return null
        if (inFlightBytes != 0 && inFlightBytes + next.bytes.size > configuration.windowBytes) return null
        val item = popNext()
        inFlightBytes += item.bytes.size
        return item
    }

    /** The connection finished writing [bytes] previously returned by [dequeue]. */
    fun acknowledge(bytes: Int) {
        inFlightBytes = (inFlightBytes - bytes).coerceAtLeast(0)
    }

    /** Drops queued video, used when a new `VideoConfig` makes older frames meaningless. */
    fun purgeVideo() {
        video.clear()
        droppingUntilKeyframe = false
    }

    /** Forgets everything, for a new connection. */
    fun reset() {
        control.clear()
        audio.clear()
        video.clear()
        logs.clear()
        inFlightBytes = 0
        queuedAudioBytes = 0
        droppingUntilKeyframe = false
    }

    private fun enqueueVideo(item: OutboundItem): EnqueueResult {
        var dropped = 0
        if (droppingUntilKeyframe) {
            if (!item.isVideoKeyframe) return EnqueueResult(droppedVideoFrames = 1)
            droppingUntilKeyframe = false
        }
        video.addLast(item)
        if (video.size <= configuration.maxQueuedVideoFrames) return EnqueueResult()

        // 1. Disposable frames are never referenced: shedding them costs nothing.
        var excess = video.size - configuration.maxQueuedVideoFrames
        val kept = ArrayDeque<OutboundItem>(video.size)
        for (candidate in video) {
            if (excess > 0 && candidate.isDisposableVideo) {
                excess--
                dropped++
            } else {
                kept.addLast(candidate)
            }
        }
        video.clear()
        video.addAll(kept)
        if (video.size <= configuration.maxQueuedVideoFrames) return EnqueueResult(droppedVideoFrames = dropped)

        // 2. Dropping a reference frame breaks every later frame until the next IDR.
        return if (item.isVideoKeyframe) {
            // The new IDR (appended last) supersedes everything queued before it.
            dropped += video.size - 1
            video.clear()
            video.addLast(item)
            EnqueueResult(droppedVideoFrames = dropped)
        } else {
            dropped += video.size
            video.clear()
            droppingUntilKeyframe = true
            EnqueueResult(droppedVideoFrames = dropped, keyframeNeeded = true)
        }
    }

    private fun peekNext(): OutboundItem? = control.firstOrNull() ?: audio.firstOrNull() ?: video.firstOrNull() ?: logs.firstOrNull()

    private fun popNext(): OutboundItem {
        control.removeFirstOrNull()?.let { return it }
        audio.removeFirstOrNull()?.let {
            queuedAudioBytes -= it.bytes.size
            return it
        }
        video.removeFirstOrNull()?.let { return it }
        return logs.removeFirst()
    }
}

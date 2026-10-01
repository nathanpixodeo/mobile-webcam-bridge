package dev.mobilewebcambridge.android.media

import dev.mobilewebcambridge.android.capture.AudioSampleSource
import dev.mobilewebcambridge.android.capture.AudioSourceListener
import dev.mobilewebcambridge.android.capture.CaptureException
import dev.mobilewebcambridge.android.capture.SourceEvent
import dev.mobilewebcambridge.android.core.AppLogger
import dev.mobilewebcambridge.android.core.SerialThread
import dev.mobilewebcambridge.protocol.media.AudioChunk
import dev.mobilewebcambridge.protocol.messages.AudioConfigMessage
import dev.mobilewebcambridge.protocol.messages.AudioStreamStatus
import dev.mobilewebcambridge.protocol.messages.ErrorCode
import dev.mobilewebcambridge.protocol.messages.ErrorMessage
import dev.mobilewebcambridge.protocol.messages.StartAudioMessage
import dev.mobilewebcambridge.protocol.messages.StreamState

/**
 * Owns the audio pipeline: sample source → chunks of at most 40 ms → session. Mutable state lives
 * on [thread]; public methods and source callbacks hop there.
 */
class AudioStreamController(
    private val thread: SerialThread,
    private val microphone: AudioSampleSource,
    private val synthetic: AudioSampleSource,
    private val sink: MediaEventSink,
    private val logger: AppLogger,
) {
    // Audio-thread state.
    private var request: StartAudioMessage? = null
    private var useSynthetic = false
    private var activeSource: AudioSampleSource? = null
    private var lastConfigId = 0
    private var reportedRunning = false

    /** A silenced recording keeps delivering (zero) samples; it must not read as running. */
    private var interrupted = false

    /** Bumped by every teardown; callbacks from older sources are ignored. */
    private var generation = 0

    fun start(request: StartAudioMessage) = thread.post {
        this.request = request
        restart()
    }

    fun stop() = thread.post {
        request = null
        tearDown()
        sink.audioStatusChanged(AudioStreamStatus.OFF)
    }

    fun setSyntheticSource(enabled: Boolean) = thread.post {
        if (useSynthetic == enabled) return@post
        useSynthetic = enabled
        if (request != null) restart()
    }

    private fun restart() {
        tearDown()
        val request = request ?: return
        sink.audioStatusChanged(AudioStreamStatus(StreamState.STARTING))
        val source = if (useSynthetic) synthetic else microphone
        val pipeline = generation
        val configId = ++lastConfigId
        // SPEC §3.4: the AudioConfig goes out before the first chunk.
        sink.audioConfigured(AudioConfigMessage(configId = configId))
        try {
            source.start(
                request,
                object : AudioSourceListener {
                    override fun onAudioCaptured(pcm: ByteArray, timestampUs: Long, discontinuity: Boolean) {
                        thread.post { captured(pipeline, configId, pcm, timestampUs, discontinuity) }
                    }

                    override fun onAudioEvent(event: SourceEvent) {
                        thread.post { onSourceEvent(pipeline, event) }
                    }
                },
            )
            activeSource = source
        } catch (error: CaptureException) {
            fail(error.error)
        } catch (error: Exception) {
            fail(ErrorMessage(ErrorCode.MIC_UNAVAILABLE, "Audio pipeline failed: ${error.message}", fatal = false))
        }
    }

    private fun captured(pipeline: Int, configId: Int, pcm: ByteArray, timestampUs: Long, discontinuity: Boolean) {
        if (pipeline != generation) return
        if (!reportedRunning && !interrupted) {
            reportedRunning = true
            sink.audioStatusChanged(AudioStreamStatus(StreamState.RUNNING))
        }
        AudioChunk.split(pcm, configId, timestampUs, discontinuity).forEach { sink.audioCaptured(it) }
    }

    private fun onSourceEvent(pipeline: Int, event: SourceEvent) {
        if (pipeline != generation) return
        when (event) {
            is SourceEvent.Interrupted -> {
                logger.warn("audio", "Microphone interrupted: ${event.reason}")
                interrupted = true
                reportedRunning = false
                sink.audioStatusChanged(AudioStreamStatus(StreamState.INTERRUPTED, reason = event.reason))
            }
            SourceEvent.Resumed -> {
                logger.info("audio", "Microphone resumed")
                interrupted = false // the next chunk reports running
            }
            is SourceEvent.Failed -> fail(ErrorMessage(ErrorCode.MIC_UNAVAILABLE, event.detail, fatal = false))
            is SourceEvent.FormatChanged -> Unit
        }
    }

    private fun fail(error: ErrorMessage) {
        logger.error("audio", "Audio failed: ${error.message}")
        request = null
        tearDown()
        sink.audioStatusChanged(AudioStreamStatus(StreamState.ERROR, reason = error.message))
        sink.audioFailed(error)
    }

    private fun tearDown() {
        generation += 1
        activeSource?.stop()
        activeSource = null
        reportedRunning = false
        interrupted = false
    }
}

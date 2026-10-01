package dev.mobilewebcambridge.android.media

import dev.mobilewebcambridge.android.capture.CaptureException
import dev.mobilewebcambridge.android.capture.SourceEvent
import dev.mobilewebcambridge.android.capture.VideoCaptureFormat
import dev.mobilewebcambridge.android.capture.VideoCaptureRequest
import dev.mobilewebcambridge.android.capture.VideoFrameSource
import dev.mobilewebcambridge.android.core.AppLogger
import dev.mobilewebcambridge.android.core.SerialThread
import dev.mobilewebcambridge.android.encoding.EncoderSettings
import dev.mobilewebcambridge.android.encoding.VideoEncoder
import dev.mobilewebcambridge.android.encoding.VideoEncoderFactory
import dev.mobilewebcambridge.protocol.media.EncodedVideoFrame
import dev.mobilewebcambridge.protocol.messages.ErrorCode
import dev.mobilewebcambridge.protocol.messages.ErrorMessage
import dev.mobilewebcambridge.protocol.messages.StartVideoMessage
import dev.mobilewebcambridge.protocol.messages.StreamState
import dev.mobilewebcambridge.protocol.messages.VideoConfigMessage
import dev.mobilewebcambridge.protocol.messages.VideoStreamStatus
import dev.mobilewebcambridge.protocol.policy.EffectiveVideoRate
import dev.mobilewebcambridge.protocol.policy.ThermalLevel
import dev.mobilewebcambridge.protocol.policy.ThermalPolicy
import dev.mobilewebcambridge.protocol.transport.KeyframeLimiter

/**
 * Owns the video pipeline: frame source → encoder input surface → H.264 encoder → session.
 *
 * Mutable state lives on [thread]; public methods hop there. A rotation restarts the pipeline (the
 * frame size changes); a thermal change is applied live: the source drops frames to the new cap,
 * the encoder takes the new bitrate, and the next IDR starts a new `VideoConfig`.
 */
class VideoStreamController(
    private val thread: SerialThread,
    private val camera: VideoFrameSource,
    private val synthetic: VideoFrameSource,
    private val encoders: VideoEncoderFactory,
    private val sink: MediaEventSink,
    private val logger: AppLogger,
) {
    private val policy = ThermalPolicy()
    private val keyframes = KeyframeGate()

    // Video-thread state.
    private var request: StartVideoMessage? = null
    private var useSynthetic = false
    private var activeSource: VideoFrameSource? = null
    private var format: VideoCaptureFormat? = null
    private var encoder: VideoEncoder? = null
    private var rate: EffectiveVideoRate? = null
    private var lastConfigId = 0
    private var thermal = ThermalLevel.NOMINAL
    private var pausedForThermal = false

    /** Bumped by every teardown; callbacks from older pipelines are ignored. */
    private var generation = 0

    fun start(request: StartVideoMessage) = thread.post {
        this.request = request
        restartPipeline()
    }

    fun stop() = thread.post {
        request = null
        pausedForThermal = false
        tearDown()
        sink.videoStatusChanged(VideoStreamStatus.OFF)
    }

    /** Any thread; coalesced and rate-limited to 2 IDRs per second (SPEC §3.3). */
    fun requestKeyframe() = keyframes.request()

    fun setSyntheticSource(enabled: Boolean) = thread.post {
        if (useSynthetic == enabled) return@post
        useSynthetic = enabled
        if (request != null) restartPipeline()
    }

    fun setThermalLevel(level: ThermalLevel) = thread.post {
        if (thermal == level) return@post
        thermal = level
        thermalLevelChanged()
    }

    // ---- Pipeline ---------------------------------------------------------------------------

    private fun restartPipeline() {
        tearDown()
        val request = request ?: return
        val throttle = policy.throttle(thermal)
        if (!throttle.videoAllowed) {
            pausedForThermal = true
            sink.videoStatusChanged(VideoStreamStatus(StreamState.INTERRUPTED, reason = "thermal"))
            return
        }
        pausedForThermal = false
        sink.videoStatusChanged(VideoStreamStatus(StreamState.STARTING))

        val source = if (useSynthetic) synthetic else camera
        val pipeline = generation
        try {
            val format = source.prepare(VideoCaptureRequest.from(request))
            val rate = policy.apply(throttle, fps = format.fps, bitrateKbps = request.bitrateKbps)
            val configId = ++lastConfigId
            val encoder = encoders.create(
                configId,
                EncoderSettings(format.width, format.height, rate.fps, rate.bitrateKbps, request.encoder),
                output = { frame -> onEncoded(frame) },
                failure = { detail -> thread.post { encoderFailed(pipeline, detail) } },
            )
            this.encoder = encoder
            this.format = format
            this.rate = rate
            keyframes.reset() // a new encoder starts with an IDR anyway
            // SPEC §3.3: the VideoConfig goes out before any access unit of its configuration.
            sink.videoConfigured(videoConfig(configId, format, rate, encoder))
            activeSource = source
            source.start(
                encoder.inputSurface,
                rate.fps,
                hook = { timestampUs -> if (keyframes.shouldForce(timestampUs)) encoder.requestSyncFrame() },
                listener = { event -> thread.post { onSourceEvent(pipeline, event) } },
            )
            reportRunning()
        } catch (error: CaptureException) {
            fail(error.error)
        } catch (error: Exception) {
            fail(ErrorMessage(ErrorCode.CAMERA_UNAVAILABLE, "Video pipeline failed: ${error.message}", fatal = false))
        }
    }

    private fun thermalLevelChanged() {
        val request = request ?: return
        val level = thermal.name.lowercase()
        val throttle = policy.throttle(thermal)
        if (!throttle.videoAllowed) {
            if (pausedForThermal) return
            logger.warn("video", "Video paused: thermal level $level")
            tearDown()
            pausedForThermal = true
            sink.videoStatusChanged(VideoStreamStatus(StreamState.INTERRUPTED, reason = "thermal"))
            return
        }
        if (pausedForThermal) {
            logger.info("video", "Thermal level $level: resuming video")
            restartPipeline()
            return
        }
        val encoder = encoder ?: return
        val format = format ?: return
        val updated = policy.apply(throttle, fps = format.fps, bitrateKbps = request.bitrateKbps)
        if (updated == rate) return
        logger.info("video", "Thermal level $level: ${updated.fps} fps, ${updated.bitrateKbps} kbps")
        rate = updated
        val configId = ++lastConfigId
        // The VideoConfig is handed to the session before the encoder can emit the IDR that
        // starts the configuration; frames still carrying the old id are dropped by the session.
        sink.videoConfigured(videoConfig(configId, format, updated, encoder))
        encoder.reconfigure(configId, updated.bitrateKbps)
        activeSource?.setFrameRate(updated.fps)
        reportRunning()
    }

    // ---- Callbacks --------------------------------------------------------------------------

    /** Encoder thread. */
    private fun onEncoded(frame: EncodedVideoFrame) {
        if (frame.isKeyframe) keyframes.noteKeyframe(frame.presentationTimeUs)
        sink.videoEncoded(frame)
    }

    private fun onSourceEvent(pipeline: Int, event: SourceEvent) {
        if (pipeline != generation) return
        when (event) {
            is SourceEvent.Interrupted -> {
                logger.warn("video", "Capture interrupted: ${event.reason}")
                sink.videoStatusChanged(VideoStreamStatus(StreamState.INTERRUPTED, reason = event.reason))
            }
            SourceEvent.Resumed -> {
                logger.info("video", "Capture resumed")
                keyframes.request()
                reportRunning()
            }
            is SourceEvent.Failed -> fail(ErrorMessage(ErrorCode.CAMERA_UNAVAILABLE, event.detail, fatal = false))
            is SourceEvent.FormatChanged -> if (event.format != format) {
                val changed = event.format
                logger.info("video", "Phone rotated: restarting at ${changed.width}x${changed.height}, rotation ${changed.rotationDegrees}")
                restartPipeline()
            }
        }
    }

    private fun encoderFailed(pipeline: Int, detail: String) {
        if (pipeline != generation) return
        fail(ErrorMessage(ErrorCode.ENCODER_FAILURE, detail, fatal = false))
    }

    private fun fail(error: ErrorMessage) {
        logger.error("video", "Video failed: ${error.message}")
        request = null
        tearDown()
        sink.videoStatusChanged(VideoStreamStatus(StreamState.ERROR, reason = error.message))
        sink.videoFailed(error)
    }

    /** Source first: it draws into the encoder's surface until it stops. */
    private fun tearDown() {
        generation += 1
        activeSource?.stop()
        activeSource = null
        encoder?.release()
        encoder = null
        format = null
        rate = null
    }

    private fun reportRunning() {
        val format = format ?: return
        val rate = rate ?: return
        sink.videoStatusChanged(
            VideoStreamStatus(
                state = StreamState.RUNNING,
                width = format.width,
                height = format.height,
                fps = rate.fps,
                bitrateKbps = rate.bitrateKbps,
            ),
        )
    }

    private fun videoConfig(configId: Int, format: VideoCaptureFormat, rate: EffectiveVideoRate, encoder: VideoEncoder) =
        VideoConfigMessage(
            configId = configId,
            profile = encoder.profileName,
            width = format.width,
            height = format.height,
            fps = rate.fps,
            bitrateKbps = rate.bitrateKbps,
            rotationDeg = format.rotationDegrees,
            mirrored = format.mirrored,
            encoder = encoder.effectiveMode,
            camera = format.camera,
        )
}

/**
 * [KeyframeLimiter] shared by the session (requests), render (frames about to be encoded) and
 * encoder (IDRs produced) threads.
 */
private class KeyframeGate {
    private val limiter = KeyframeLimiter()

    @Synchronized
    fun request() = limiter.request()

    @Synchronized
    fun shouldForce(timestampUs: Long): Boolean = limiter.shouldForceKeyframe(timestampUs)

    @Synchronized
    fun noteKeyframe(timestampUs: Long) = limiter.noteKeyframe(timestampUs)

    @Synchronized
    fun reset() = limiter.reset()
}

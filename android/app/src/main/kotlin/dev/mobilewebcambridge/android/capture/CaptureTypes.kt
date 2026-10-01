package dev.mobilewebcambridge.android.capture

import android.view.Surface
import dev.mobilewebcambridge.protocol.messages.CameraSelection
import dev.mobilewebcambridge.protocol.messages.ErrorCode
import dev.mobilewebcambridge.protocol.messages.ErrorMessage
import dev.mobilewebcambridge.protocol.messages.OrientationMode
import dev.mobilewebcambridge.protocol.messages.StartAudioMessage
import dev.mobilewebcambridge.protocol.messages.StartVideoMessage

/** What the session asks a video source for. Sizes are in landscape terms (SPEC §4). */
data class VideoCaptureRequest(
    val width: Int,
    val height: Int,
    val fps: Int,
    val camera: CameraSelection,
    val mirror: Boolean,
    val orientation: OrientationMode,
) {
    companion object {
        fun from(message: StartVideoMessage): VideoCaptureRequest = VideoCaptureRequest(
            width = maxOf(message.width, message.height),
            height = minOf(message.width, message.height),
            fps = message.fps,
            camera = message.camera,
            mirror = message.mirror,
            orientation = message.orientation,
        )
    }
}

/**
 * What a video source delivers: encoded frame size after rotation, the highest frame rate it can
 * reach for the request, and the camera actually used.
 */
data class VideoCaptureFormat(
    val width: Int,
    val height: Int,
    val fps: Int,
    val rotationDegrees: Int,
    val mirrored: Boolean,
    val camera: CameraSelection,
)

/** A capture failure carrying the wire error to report. */
class CaptureException(val error: ErrorMessage) : Exception(error.message) {
    companion object {
        fun permissionDenied(detail: String) = CaptureException(ErrorMessage(ErrorCode.PERMISSION_DENIED, detail, fatal = false))

        fun cameraUnavailable(detail: String) = CaptureException(ErrorMessage(ErrorCode.CAMERA_UNAVAILABLE, detail, fatal = false))

        fun micUnavailable(detail: String) = CaptureException(ErrorMessage(ErrorCode.MIC_UNAVAILABLE, detail, fatal = false))

        fun encoderFailure(detail: String) = CaptureException(ErrorMessage(ErrorCode.ENCODER_FAILURE, detail, fatal = false))
    }
}

sealed interface SourceEvent {
    data class Interrupted(val reason: String) : SourceEvent

    data object Resumed : SourceEvent

    data class Failed(val detail: String) : SourceEvent

    /** The source would now produce a different format (e.g. the phone was rotated). */
    data class FormatChanged(val format: VideoCaptureFormat) : SourceEvent
}

/** Called on the render thread right before a frame is drawn into the encoder surface. */
fun interface FrameHook {
    fun beforeFrame(timestampUs: Long)
}

/** Source callbacks; any thread. */
fun interface VideoSourceListener {
    fun onSourceEvent(event: SourceEvent)
}

/** Produces frames by drawing into the encoder's input surface. Called on the video thread. */
interface VideoFrameSource {
    /** Resolves camera, orientation and sizes for [request] without starting capture. */
    fun prepare(request: VideoCaptureRequest): VideoCaptureFormat

    /** Starts drawing frames of the prepared format into [target], at most [fps] per second. */
    fun start(target: Surface, fps: Int, hook: FrameHook, listener: VideoSourceListener)

    /** Changes the frame-rate cap of the running source (thermal throttling) without a restart. */
    fun setFrameRate(fps: Int)

    fun stop()
}

/** Audio callbacks; any thread. */
interface AudioSourceListener {
    /** 48 kHz mono `s16le`; [timestampUs] is the first sample's capture time (device clock). */
    fun onAudioCaptured(pcm: ByteArray, timestampUs: Long, discontinuity: Boolean)

    fun onAudioEvent(event: SourceEvent)
}

/** Produces 48 kHz mono `s16le`. Called on the audio thread. */
interface AudioSampleSource {
    fun start(request: StartAudioMessage, listener: AudioSourceListener)

    fun stop()
}

package dev.mobilewebcambridge.android.media

import dev.mobilewebcambridge.protocol.media.AudioChunk
import dev.mobilewebcambridge.protocol.media.EncodedVideoFrame
import dev.mobilewebcambridge.protocol.messages.AudioConfigMessage
import dev.mobilewebcambridge.protocol.messages.AudioStreamStatus
import dev.mobilewebcambridge.protocol.messages.ErrorMessage
import dev.mobilewebcambridge.protocol.messages.StartAudioMessage
import dev.mobilewebcambridge.protocol.messages.StartVideoMessage
import dev.mobilewebcambridge.protocol.messages.VideoConfigMessage
import dev.mobilewebcambridge.protocol.messages.VideoStreamStatus

/** Commands from the session to the media pipelines. Implementations hop to their own threads. */
interface MediaControlling {
    fun startVideo(request: StartVideoMessage)

    fun stopVideo()

    fun requestKeyframe()

    fun startAudio(request: StartAudioMessage)

    fun stopAudio()
}

/**
 * Outputs of the media pipelines, called from the video, audio, encoder and capture threads with
 * immutable values; the session hops them onto its own thread.
 */
interface MediaEventSink {
    fun videoConfigured(config: VideoConfigMessage)

    fun videoEncoded(frame: EncodedVideoFrame)

    fun videoStatusChanged(status: VideoStreamStatus)

    fun videoFailed(error: ErrorMessage)

    fun audioConfigured(config: AudioConfigMessage)

    fun audioCaptured(chunk: AudioChunk)

    fun audioStatusChanged(status: AudioStreamStatus)

    fun audioFailed(error: ErrorMessage)
}

/** Routes session commands to the two pipelines. */
class MediaController(
    private val video: VideoStreamController,
    private val audio: AudioStreamController,
) : MediaControlling {
    override fun startVideo(request: StartVideoMessage) = video.start(request)

    override fun stopVideo() = video.stop()

    override fun requestKeyframe() = video.requestKeyframe()

    override fun startAudio(request: StartAudioMessage) = audio.start(request)

    override fun stopAudio() = audio.stop()
}

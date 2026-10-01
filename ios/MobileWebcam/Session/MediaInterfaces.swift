import BridgeKit

/// Commands from the session to the media pipelines. Implementations hop to their own queues.
protocol MediaControlling: AnyObject, Sendable {
    func startVideo(_ request: StartVideoMessage)
    func stopVideo()
    func requestKeyframe()
    func startAudio(_ request: StartAudioMessage)
    func stopAudio()
}

/// Outputs of the media pipelines. Called from capture, encoder and VideoToolbox threads with
/// Sendable values; the session hops them onto its queue.
protocol MediaEventSink: AnyObject, Sendable {
    func videoConfigured(_ config: VideoConfigMessage)
    func videoEncoded(_ frame: EncodedVideoFrame)
    func videoStatusChanged(_ status: VideoStreamStatus)
    func videoFailed(_ error: ErrorMessage)
    func audioConfigured(_ config: AudioConfigMessage)
    func audioCaptured(_ chunk: AudioChunk)
    func audioStatusChanged(_ status: AudioStreamStatus)
    func audioFailed(_ error: ErrorMessage)
}

/// Routes session commands to the two pipelines.
final class MediaController: MediaControlling {
    private let video: VideoStreamController
    private let audio: AudioStreamController

    init(video: VideoStreamController, audio: AudioStreamController) {
        self.video = video
        self.audio = audio
    }

    func startVideo(_ request: StartVideoMessage) { video.start(request) }
    func stopVideo() { video.stop() }
    func requestKeyframe() { video.requestKeyframe() }
    func startAudio(_ request: StartAudioMessage) { audio.start(request) }
    func stopAudio() { audio.stop() }
}

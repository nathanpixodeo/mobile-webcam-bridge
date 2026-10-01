import BridgeKit
import Foundation

/// Owns the audio pipeline: sample source → 40 ms chunks → session. Mutable state lives on
/// `queue`; public methods hop there.
final class AudioStreamController: AudioSampleSourceDelegate, @unchecked Sendable {
    private let queue: DispatchQueue
    private let microphone: any AudioSampleSource
    private let synthetic: any AudioSampleSource
    private let logger: AppLogger
    private weak var sink: (any MediaEventSink)?

    private var request: StartAudioMessage?
    private var useSynthetic = false
    private var activeSource: (any AudioSampleSource)?
    private var configId = 0
    private var hasReportedRunning = false

    init(queue: DispatchQueue, microphone: any AudioSampleSource, synthetic: any AudioSampleSource,
         sink: any MediaEventSink, logger: AppLogger) {
        self.queue = queue
        self.microphone = microphone
        self.synthetic = synthetic
        self.sink = sink
        self.logger = logger
    }

    // MARK: - Commands (any thread)

    func start(_ request: StartAudioMessage) {
        queue.async { [self] in
            self.request = request
            restart()
        }
    }

    func stop() {
        queue.async { [self] in
            request = nil
            tearDown()
            sink?.audioStatusChanged(.off)
        }
    }

    func setSyntheticSource(_ enabled: Bool) {
        queue.async { [self] in
            guard useSynthetic != enabled else { return }
            useSynthetic = enabled
            if request != nil { restart() }
        }
    }

    // MARK: - AudioSampleSourceDelegate (audio queue)

    func audioSource(didCapture pcm: Data, timestampUs: Int64, discontinuity: Bool) {
        guard activeSource != nil else { return }
        if !hasReportedRunning {
            hasReportedRunning = true
            sink?.audioStatusChanged(AudioStreamStatus(state: .running))
        }
        for chunk in AudioChunk.split(pcm: pcm, configId: configId, timestampUs: timestampUs,
                                      isDiscontinuity: discontinuity) {
            sink?.audioCaptured(chunk)
        }
    }

    func audioSource(didEmit event: SourceEvent) {
        switch event {
        case .interrupted(let reason):
            logger.warn("audio", "Microphone interrupted: \(reason)")
            hasReportedRunning = false
            sink?.audioStatusChanged(AudioStreamStatus(state: .interrupted, reason: reason))
        case .resumed:
            logger.info("audio", "Microphone resumed")
        case .failed(let detail):
            fail(ErrorMessage(code: .micUnavailable, message: detail, fatal: false))
        }
    }

    // MARK: - Pipeline

    private func restart() {
        tearDown()
        guard let request else { return }
        sink?.audioStatusChanged(AudioStreamStatus(state: .starting))
        let source = useSynthetic ? synthetic : microphone
        configId += 1
        // Announce the format before the source can deliver its first chunk.
        sink?.audioConfigured(AudioConfigMessage(configId: configId))
        do {
            try source.start(request, delegate: self)
            activeSource = source
        } catch let error as CaptureError {
            fail(ErrorMessage(code: error.errorMessage.code == .permissionDenied ? .permissionDenied : .micUnavailable,
                              message: error.errorMessage.message, fatal: false))
        } catch {
            fail(ErrorMessage(code: .micUnavailable, message: "\(error)", fatal: false))
        }
    }

    private func fail(_ error: ErrorMessage) {
        logger.error("audio", "Audio failed: \(error.message)")
        request = nil
        tearDown()
        sink?.audioStatusChanged(AudioStreamStatus(state: .error, reason: error.message))
        sink?.audioFailed(error)
    }

    private func tearDown() {
        activeSource?.stop()
        activeSource = nil
        hasReportedRunning = false
    }
}

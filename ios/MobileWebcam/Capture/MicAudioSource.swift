import AVFoundation
import BridgeKit

/// Microphone capture with an audio-only AVCaptureSession, converted to 48 kHz mono `s16le`.
/// Everything runs on the audio queue (`queue`).
///
/// Audio uses its own capture session, separate from the camera's, so it keeps running when the
/// camera is stopped or interrupted (and, with the `audio` background mode, when the screen locks).
final class MicAudioSource: NSObject, AudioSampleSource, AVCaptureAudioDataOutputSampleBufferDelegate,
    @unchecked Sendable {
    private let queue: DispatchQueue
    private let logger: AppLogger
    private let audioSession: AudioSessionController
    private let session = AVCaptureSession()
    private let output = AVCaptureAudioDataOutput()
    private var isConfigured = false

    private weak var delegate: (any AudioSampleSourceDelegate)?
    private var resampler: LinearAudioResampler?
    private var pendingDiscontinuity = true
    private var hasLoggedFormat = false
    private var notificationTokens: [any NSObjectProtocol] = []

    init(queue: DispatchQueue, audioSession: AudioSessionController, logger: AppLogger) {
        self.queue = queue
        self.audioSession = audioSession
        self.logger = logger
        super.init()
    }

    // MARK: - AudioSampleSource

    func start(_ request: StartAudioMessage, delegate: any AudioSampleSourceDelegate) throws {
        dispatchPrecondition(condition: .onQueue(queue))
        guard AVCaptureDevice.authorizationStatus(for: .audio) == .authorized else {
            throw CaptureError.permissionDenied("Microphone access is not authorized")
        }
        try audioSession.activate(processing: request.processing)
        try configureIfNeeded()

        self.delegate = delegate
        resampler = nil
        pendingDiscontinuity = true
        hasLoggedFormat = false
        observeInterruptions()
        session.startRunning()
    }

    func stop() {
        dispatchPrecondition(condition: .onQueue(queue))
        stopObserving()
        if session.isRunning { session.stopRunning() }
        audioSession.deactivate()
        delegate = nil
    }

    // MARK: - Sample delivery (audio queue)

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer,
                       from connection: AVCaptureConnection) {
        guard let description = CMSampleBufferGetFormatDescription(sampleBuffer),
              let format = CMAudioFormatDescriptionGetStreamBasicDescription(description)?.pointee
        else { return }

        let samples: [Float]
        do {
            samples = try sampleBuffer.withAudioBufferList { buffers, _ in
                try PCMSampleExtractor.monoSamples(from: buffers, format: format)
            }
        } catch {
            delegate?.audioSource(didEmit: .failed("Unsupported microphone format: \(error)"))
            return
        }
        guard !samples.isEmpty else { return }

        var converter = resampler ?? LinearAudioResampler(inputRate: format.mSampleRate,
                                                          outputRate: Double(AudioChunk.sampleRate))
        if converter.inputRate != format.mSampleRate {
            converter = LinearAudioResampler(inputRate: format.mSampleRate, outputRate: Double(AudioChunk.sampleRate))
            pendingDiscontinuity = true
        }
        if !hasLoggedFormat {
            hasLoggedFormat = true
            logger.info("audio", "Microphone delivers \(Int(format.mSampleRate)) Hz, \(format.mChannelsPerFrame) ch, "
                + "\(format.mBitsPerChannel)-bit\(converter.isPassthrough ? "" : ", resampling to 48 kHz")")
        }
        let resampled = converter.process(samples)
        resampler = converter

        let timestamp = HostTimeClock.microseconds(CMSampleBufferGetPresentationTimeStamp(sampleBuffer),
                                                   from: session.synchronizationClock)
        delegate?.audioSource(didCapture: PCM16.encodeLittleEndian(resampled), timestampUs: timestamp,
                              discontinuity: pendingDiscontinuity)
        pendingDiscontinuity = false
    }

    // MARK: - Configuration

    private func configureIfNeeded() throws {
        guard !isConfigured else { return }
        guard let microphone = AVCaptureDevice.default(for: .audio) else {
            throw CaptureError.deviceUnavailable("No microphone available")
        }
        let input: AVCaptureDeviceInput
        do {
            input = try AVCaptureDeviceInput(device: microphone)
        } catch {
            throw CaptureError.deviceUnavailable("Cannot open microphone: \(error.localizedDescription)")
        }
        session.beginConfiguration()
        defer { session.commitConfiguration() }
        session.automaticallyConfiguresApplicationAudioSession = false
        session.usesApplicationAudioSession = true
        guard session.canAddInput(input) else { throw CaptureError.configurationFailed("Cannot add microphone input") }
        session.addInput(input)
        output.setSampleBufferDelegate(self, queue: queue)
        guard session.canAddOutput(output) else { throw CaptureError.configurationFailed("Cannot add audio output") }
        session.addOutput(output)
        isConfigured = true
    }

    // MARK: - Interruptions

    private func observeInterruptions() {
        stopObserving()
        let center = NotificationCenter.default
        notificationTokens = [
            center.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: nil) { [weak self] note in
                let rawType = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt
                let began = rawType.flatMap(AVAudioSession.InterruptionType.init(rawValue:)) == .began
                guard let self else { return }
                queue.async { self.handleAudioInterruption(began: began) }
            },
            center.addObserver(forName: AVCaptureSession.wasInterruptedNotification, object: session, queue: nil) { [weak self] note in
                let reason = CameraVideoSource.interruptionDescription(note.userInfo?[AVCaptureSessionInterruptionReasonKey] as? Int)
                guard let self else { return }
                queue.async { self.delegate?.audioSource(didEmit: .interrupted(reason: reason)) }
            },
            center.addObserver(forName: AVCaptureSession.interruptionEndedNotification, object: session, queue: nil) { [weak self] _ in
                guard let self else { return }
                queue.async {
                    self.pendingDiscontinuity = true
                    self.delegate?.audioSource(didEmit: .resumed)
                }
            },
            center.addObserver(forName: AVAudioSession.mediaServicesWereResetNotification, object: nil, queue: nil) { [weak self] _ in
                guard let self else { return }
                queue.async { self.delegate?.audioSource(didEmit: .failed("Media services were reset")) }
            },
        ]
    }

    private func stopObserving() {
        notificationTokens.forEach { NotificationCenter.default.removeObserver($0) }
        notificationTokens.removeAll()
    }

    private func handleAudioInterruption(began: Bool) {
        guard delegate != nil else { return }
        if began {
            delegate?.audioSource(didEmit: .interrupted(reason: "audioInterrupted"))
            return
        }
        pendingDiscontinuity = true
        do {
            try AVAudioSession.sharedInstance().setActive(true)
            if !session.isRunning { session.startRunning() }
            delegate?.audioSource(didEmit: .resumed)
        } catch {
            delegate?.audioSource(didEmit: .failed("Cannot reactivate audio session: \(error.localizedDescription)"))
        }
    }
}

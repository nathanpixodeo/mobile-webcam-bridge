import BridgeKit
import CoreVideo
import Foundation

/// Owns the video pipeline: frame source → H.264 encoder → session.
///
/// Every piece of mutable state lives on `queue`; public methods hop there. Frames never leave
/// the queue except as encoded, Sendable `EncodedVideoFrame`s, so no `CMSampleBuffer` or
/// `CVPixelBuffer` crosses an isolation boundary.
final class VideoStreamController: VideoFrameSourceDelegate, @unchecked Sendable {
    private let queue: DispatchQueue
    private let camera: any VideoFrameSource
    private let synthetic: any VideoFrameSource
    private let logger: AppLogger
    private let policy = ThermalPolicy()
    private weak var sink: (any MediaEventSink)?

    private var request: StartVideoMessage?
    private var useSynthetic = false
    private var activeSource: (any VideoFrameSource)?
    private var format: VideoCaptureFormat?
    private var encoder: H264Encoder?
    private var keyframes = KeyframeLimiter()
    private var nextConfigId = 1
    private var processThermal: ThermalLevel = .nominal
    private var cameraPressure: ThermalLevel = .nominal
    private var pausedForThermal = false

    init(queue: DispatchQueue, camera: any VideoFrameSource, synthetic: any VideoFrameSource,
         sink: any MediaEventSink, logger: AppLogger) {
        self.queue = queue
        self.camera = camera
        self.synthetic = synthetic
        self.sink = sink
        self.logger = logger
    }

    // MARK: - Commands (any thread)

    func start(_ request: StartVideoMessage) {
        queue.async { [self] in
            self.request = request
            restartPipeline()
        }
    }

    func stop() {
        queue.async { [self] in
            request = nil
            pausedForThermal = false
            tearDownPipeline()
            sink?.videoStatusChanged(.off)
        }
    }

    func requestKeyframe() {
        queue.async { [self] in keyframes.request() }
    }

    func setSyntheticSource(_ enabled: Bool) {
        queue.async { [self] in
            guard useSynthetic != enabled else { return }
            useSynthetic = enabled
            if request != nil { restartPipeline() }
        }
    }

    func setProcessThermalLevel(_ level: ThermalLevel) {
        queue.async { [self] in
            processThermal = level
            thermalLevelChanged()
        }
    }

    // MARK: - VideoFrameSourceDelegate (video queue)

    func frameSource(didOutput pixelBuffer: CVPixelBuffer, presentationTimeUs: Int64) {
        guard let encoder else { return }
        let force = keyframes.shouldForceKeyframe(nowUs: presentationTimeUs)
        encoder.encode(pixelBuffer, presentationTimeUs: presentationTimeUs, forceKeyframe: force)
    }

    func frameSource(didChangeFormat format: VideoCaptureFormat) {
        guard request != nil, format != self.format else { return }
        logger.info("video", "Capture format changed to \(format.width)x\(format.height) rotation \(format.rotationDegrees)")
        self.format = format
        do {
            try configureEncoder()
        } catch {
            fail(.configurationFailed("Encoder reconfiguration failed: \(error)"))
        }
    }

    func frameSource(didEmit event: SourceEvent) {
        switch event {
        case .interrupted(let reason):
            logger.warn("video", "Capture interrupted: \(reason)")
            sink?.videoStatusChanged(VideoStreamStatus(state: .interrupted, reason: reason))
        case .resumed:
            logger.info("video", "Capture resumed")
            keyframes.request()
            reportRunning()
        case .failed(let detail):
            fail(.deviceUnavailable(detail))
        }
    }

    func frameSource(didChangePressure level: ThermalLevel) {
        cameraPressure = level
        thermalLevelChanged()
    }

    // MARK: - Pipeline

    private var thermalLevel: ThermalLevel { max(processThermal, cameraPressure) }

    private func restartPipeline() {
        tearDownPipeline()
        guard let request else { return }

        let throttle = policy.throttle(for: thermalLevel)
        guard throttle.videoAllowed else {
            pausedForThermal = true
            sink?.videoStatusChanged(VideoStreamStatus(state: .interrupted, reason: "thermal"))
            return
        }
        pausedForThermal = false
        sink?.videoStatusChanged(VideoStreamStatus(state: .starting))

        let source = useSynthetic ? synthetic : camera
        do {
            format = try source.start(VideoCaptureRequest(request, fpsCap: throttle.maxFps), delegate: self)
            activeSource = source
            try configureEncoder()
        } catch let error as CaptureError {
            source.stop()
            fail(error)
        } catch {
            source.stop()
            fail(.configurationFailed("\(error)"))
        }
    }

    /// Creates the encoder for the current format and announces the new `VideoConfig`. The first
    /// frame of a new VTCompressionSession is always an IDR with parameter sets.
    private func configureEncoder() throws {
        encoder?.invalidate()
        encoder = nil
        guard let request, let format else { return }

        let effective = policy.apply(policy.throttle(for: thermalLevel),
                                     fps: min(request.fps, format.fps), bitrateKbps: request.bitrateKbps)
        let configId = nextConfigId
        nextConfigId += 1
        let settings = EncoderSettings(width: format.width, height: format.height, fps: effective.fps,
                                       bitrateKbps: effective.bitrateKbps, mode: request.encoder)
        let newEncoder = try H264Encoder(
            configId: configId,
            settings: settings,
            logger: logger,
            output: { [weak sink = self.sink] frame in sink?.videoEncoded(frame) },
            failure: { [weak self] status in
                guard let self else { return }
                queue.async { self.encoderFailed(status, configId: configId) }
            }
        )
        encoder = newEncoder
        keyframes.reset()
        keyframes.request()

        sink?.videoConfigured(VideoConfigMessage(
            configId: configId,
            width: format.width,
            height: format.height,
            fps: effective.fps,
            bitrateKbps: effective.bitrateKbps,
            rotationDeg: format.rotationDegrees,
            mirrored: format.mirrored,
            encoder: newEncoder.effectiveMode,
            camera: format.camera
        ))
        reportRunning()
    }

    private func thermalLevelChanged() {
        guard request != nil else { return }
        let throttle = policy.throttle(for: thermalLevel)
        if !throttle.videoAllowed {
            guard !pausedForThermal else { return }
            logger.warn("video", "Video paused: thermal level \(thermalLevel)")
            tearDownPipeline()
            pausedForThermal = true
            sink?.videoStatusChanged(VideoStreamStatus(state: .interrupted, reason: "thermal"))
            return
        }
        if pausedForThermal {
            logger.info("video", "Thermal level \(thermalLevel): resuming video")
            restartPipeline()
            return
        }
        guard let encoder, let request, let format else { return }
        let effective = policy.apply(throttle, fps: min(request.fps, format.fps), bitrateKbps: request.bitrateKbps)
        guard effective.fps != encoder.settings.fps || effective.bitrateKbps != encoder.settings.bitrateKbps else { return }
        logger.info("video", "Thermal level \(thermalLevel): \(effective.fps) fps, \(effective.bitrateKbps) kbps")
        activeSource?.applyFrameRateCap(effective.fps)
        do {
            try configureEncoder()
        } catch {
            fail(.configurationFailed("Encoder reconfiguration failed: \(error)"))
        }
    }

    private func encoderFailed(_ status: OSStatus, configId: Int) {
        guard encoder?.configId == configId else { return }
        logger.error("video", "Encoder failed with status \(status)")
        fail(.encoderFailed(status))
    }

    private func fail(_ error: CaptureError) {
        logger.error("video", "Video failed: \(error)")
        request = nil
        tearDownPipeline()
        sink?.videoStatusChanged(VideoStreamStatus(state: .error, reason: error.errorMessage.message))
        sink?.videoFailed(error.errorMessage)
    }

    private func tearDownPipeline() {
        activeSource?.stop()
        activeSource = nil
        encoder?.invalidate()
        encoder = nil
        format = nil
    }

    private func reportRunning() {
        guard let encoder else { return }
        sink?.videoStatusChanged(VideoStreamStatus(
            state: .running,
            width: encoder.settings.width,
            height: encoder.settings.height,
            fps: encoder.settings.fps,
            bitrateKbps: encoder.settings.bitrateKbps
        ))
    }
}

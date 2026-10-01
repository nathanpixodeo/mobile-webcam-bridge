import AVFoundation
import BridgeKit

/// Camera capture with AVFoundation. Session configuration, sample delivery and every delegate
/// callback happen on the video queue (`queue`), so the class needs no locks.
final class CameraVideoSource: NSObject, VideoFrameSource, AVCaptureVideoDataOutputSampleBufferDelegate,
    @unchecked Sendable {
    private let queue: DispatchQueue
    private let logger: AppLogger
    private let selector = FormatSelector()
    private let session = AVCaptureSession()
    private let output = AVCaptureVideoDataOutput()
    private var input: AVCaptureDeviceInput?
    private var isOutputAttached = false

    private weak var delegate: (any VideoFrameSourceDelegate)?
    private var request: VideoCaptureRequest?
    private var format: VideoCaptureFormat?
    private var rotationCoordinator: AVCaptureDevice.RotationCoordinator?
    private var observations: [NSKeyValueObservation] = []
    private var notificationTokens: [any NSObjectProtocol] = []

    init(queue: DispatchQueue, logger: AppLogger) {
        self.queue = queue
        self.logger = logger
        super.init()
    }

    // MARK: - VideoFrameSource

    func start(_ request: VideoCaptureRequest, delegate: any VideoFrameSourceDelegate) throws -> VideoCaptureFormat {
        dispatchPrecondition(condition: .onQueue(queue))
        guard AVCaptureDevice.authorizationStatus(for: .video) == .authorized else {
            throw CaptureError.permissionDenied("Camera access is not authorized")
        }
        guard let resolved = Self.resolveDevice(request.camera) else {
            throw CaptureError.deviceUnavailable("No camera available")
        }
        let (device, camera) = resolved
        if camera != request.camera {
            logger.warn("video", "Camera \(request.camera.rawValue) unavailable, using \(camera.rawValue)")
        }

        session.beginConfiguration()
        let captureSize: (width: Int, height: Int, fps: Int)
        do {
            try attach(device)
            captureSize = try configure(device, for: request)
        } catch {
            session.commitConfiguration()
            throw error
        }
        let rotation = rotationAngle(for: request.orientation, device: device)
        applyConnectionSettings(rotation: rotation, mirror: request.mirror)
        session.commitConfiguration()

        self.delegate = delegate
        self.request = request
        let format = Self.format(width: captureSize.width, height: captureSize.height, fps: captureSize.fps,
                                 rotation: rotation, mirrored: request.mirror && output.connection(with: .video)?.isVideoMirrored == true,
                                 camera: camera)
        self.format = format
        observe(device: device, orientation: request.orientation)
        session.startRunning()
        logger.info("video", "Camera \(camera.rawValue) running at \(captureSize.width)x\(captureSize.height)@\(captureSize.fps)")
        return format
    }

    func stop() {
        dispatchPrecondition(condition: .onQueue(queue))
        stopObserving()
        if session.isRunning { session.stopRunning() }
        delegate = nil
        request = nil
        format = nil
    }

    func applyFrameRateCap(_ fps: Int) {
        dispatchPrecondition(condition: .onQueue(queue))
        guard let device = input?.device else { return }
        do {
            try device.lockForConfiguration()
            defer { device.unlockForConfiguration() }
            let rate = Self.supportedRate(fps, in: device.activeFormat)
            let duration = CMTime(value: 1, timescale: CMTimeScale(rate))
            device.activeVideoMinFrameDuration = duration
            device.activeVideoMaxFrameDuration = duration
            format?.fps = rate
        } catch {
            logger.warn("video", "Could not change frame rate: \(error)")
        }
    }

    // MARK: - Sample delivery (video queue)

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer,
                       from connection: AVCaptureConnection) {
        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let presentationTime = HostTimeClock.microseconds(CMSampleBufferGetPresentationTimeStamp(sampleBuffer),
                                                          from: session.synchronizationClock)
        delegate?.frameSource(didOutput: pixelBuffer, presentationTimeUs: presentationTime)
    }

    // MARK: - Configuration

    private func attach(_ device: AVCaptureDevice) throws {
        if input?.device != device {
            if let input { session.removeInput(input) }
            let newInput: AVCaptureDeviceInput
            do {
                newInput = try AVCaptureDeviceInput(device: device)
            } catch {
                throw CaptureError.deviceUnavailable("Cannot open camera: \(error.localizedDescription)")
            }
            guard session.canAddInput(newInput) else { throw CaptureError.configurationFailed("Cannot add camera input") }
            session.addInput(newInput)
            input = newInput
        }
        if !isOutputAttached {
            output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange]
            output.alwaysDiscardsLateVideoFrames = true
            output.setSampleBufferDelegate(self, queue: queue)
            guard session.canAddOutput(output) else { throw CaptureError.configurationFailed("Cannot add video output") }
            session.addOutput(output)
            isOutputAttached = true
        }
        session.sessionPreset = .inputPriority
        session.automaticallyConfiguresCaptureDeviceForWideColor = false
    }

    private func configure(_ device: AVCaptureDevice,
                           for request: VideoCaptureRequest) throws -> (width: Int, height: Int, fps: Int) {
        let infos = device.formats.enumerated().map { index, format in Self.describe(format, index: index) }
        guard let choice = selector.select(from: infos, width: request.width, height: request.height, fps: request.fps) else {
            throw CaptureError.configurationFailed("No usable capture format")
        }
        let format = device.formats[choice.index]
        let rate = Self.supportedRate(request.fps, in: format)

        do {
            try device.lockForConfiguration()
        } catch {
            throw CaptureError.configurationFailed("Cannot lock camera: \(error.localizedDescription)")
        }
        defer { device.unlockForConfiguration() }
        device.activeFormat = format
        let duration = CMTime(value: 1, timescale: CMTimeScale(rate))
        device.activeVideoMinFrameDuration = duration
        device.activeVideoMaxFrameDuration = duration
        if format.isVideoHDRSupported {
            device.automaticallyAdjustsVideoHDREnabled = false
            device.isVideoHDREnabled = false
        }
        // sRGB is the camera's Rec. 709 mode; the encoder tags its output as BT.709.
        if format.supportedColorSpaces.contains(.sRGB) {
            device.activeColorSpace = .sRGB
        }
        return (choice.width, choice.height, rate)
    }

    private func applyConnectionSettings(rotation: CGFloat, mirror: Bool) {
        guard let connection = output.connection(with: .video) else { return }
        if connection.isVideoRotationAngleSupported(rotation) {
            connection.videoRotationAngle = rotation
        }
        if connection.isVideoMirroringSupported {
            connection.automaticallyAdjustsVideoMirroring = false
            connection.isVideoMirrored = mirror
        }
    }

    private func rotationAngle(for orientation: OrientationMode, device: AVCaptureDevice) -> CGFloat {
        switch orientation {
        case .landscape:
            return 0
        case .portrait:
            return 90
        case .auto:
            if rotationCoordinator?.device != device {
                rotationCoordinator = AVCaptureDevice.RotationCoordinator(device: device, previewLayer: nil)
            }
            return rotationCoordinator?.videoRotationAngleForHorizonLevelCapture ?? 0
        }
    }

    /// Called when the device is rotated in `auto` mode.
    private func applyRotation(_ angle: CGFloat) {
        guard let current = format, let request, request.orientation == .auto,
              let connection = output.connection(with: .video) else { return }
        let normalized = Self.normalize(angle)
        guard Int(normalized) != current.rotationDegrees, connection.isVideoRotationAngleSupported(normalized) else { return }
        connection.videoRotationAngle = normalized

        let isPortrait = Int(normalized) % 180 != 0
        let wasPortrait = current.rotationDegrees % 180 != 0
        var updated = current
        if isPortrait != wasPortrait {
            updated.width = current.height
            updated.height = current.width
        }
        updated.rotationDegrees = Int(normalized)
        format = updated
        delegate?.frameSource(didChangeFormat: updated)
    }

    // MARK: - Observation

    private func observe(device: AVCaptureDevice, orientation: OrientationMode) {
        stopObserving()
        if orientation == .auto, let coordinator = rotationCoordinator {
            observations.append(coordinator.observe(\.videoRotationAngleForHorizonLevelCapture, options: [.new]) { [weak self] coordinator, _ in
                let angle = coordinator.videoRotationAngleForHorizonLevelCapture
                guard let self else { return }
                queue.async { self.applyRotation(angle) }
            })
        }
        observations.append(device.observe(\.systemPressureState, options: [.initial, .new]) { [weak self] device, _ in
            let level = ThermalLevel(device.systemPressureState.level)
            guard let self else { return }
            queue.async { self.delegate?.frameSource(didChangePressure: level) }
        })

        let center = NotificationCenter.default
        notificationTokens = [
            center.addObserver(forName: AVCaptureSession.wasInterruptedNotification, object: session, queue: nil) { [weak self] note in
                let reason = CameraVideoSource.interruptionDescription(note.userInfo?[AVCaptureSessionInterruptionReasonKey] as? Int)
                guard let self else { return }
                queue.async { self.delegate?.frameSource(didEmit: .interrupted(reason: reason)) }
            },
            center.addObserver(forName: AVCaptureSession.interruptionEndedNotification, object: session, queue: nil) { [weak self] _ in
                guard let self else { return }
                queue.async { self.delegate?.frameSource(didEmit: .resumed) }
            },
            center.addObserver(forName: AVCaptureSession.runtimeErrorNotification, object: session, queue: nil) { [weak self] note in
                let error = note.userInfo?[AVCaptureSessionErrorKey] as? NSError
                let mediaServicesReset = error?.code == AVError.Code.mediaServicesWereReset.rawValue
                let detail = error?.localizedDescription ?? "unknown capture error"
                guard let self else { return }
                queue.async { self.handleRuntimeError(detail: detail, mediaServicesReset: mediaServicesReset) }
            },
        ]
    }

    private func stopObserving() {
        observations.forEach { $0.invalidate() }
        observations.removeAll()
        notificationTokens.forEach { NotificationCenter.default.removeObserver($0) }
        notificationTokens.removeAll()
    }

    private func handleRuntimeError(detail: String, mediaServicesReset: Bool) {
        guard request != nil else { return }
        logger.error("video", "Capture runtime error: \(detail)")
        if mediaServicesReset {
            // The session survives a media services reset; restarting it is enough.
            session.startRunning()
            delegate?.frameSource(didEmit: .resumed)
        } else {
            delegate?.frameSource(didEmit: .failed(detail))
        }
    }

    // MARK: - Helpers

    private static func resolveDevice(_ selection: CameraSelection) -> (AVCaptureDevice, CameraSelection)? {
        let (type, position): (AVCaptureDevice.DeviceType, AVCaptureDevice.Position) = switch selection {
        case .backWide: (.builtInWideAngleCamera, .back)
        case .backUltraWide: (.builtInUltraWideCamera, .back)
        case .backTelephoto: (.builtInTelephotoCamera, .back)
        case .front: (.builtInWideAngleCamera, .front)
        }
        if let device = AVCaptureDevice.default(type, for: .video, position: position) {
            return (device, selection)
        }
        if let fallback = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back) {
            return (fallback, .backWide)
        }
        return nil
    }

    private static func describe(_ format: AVCaptureDevice.Format, index: Int) -> CaptureFormatInfo {
        let dimensions = CMVideoFormatDescriptionGetDimensions(format.formatDescription)
        let ranges = format.videoSupportedFrameRateRanges
        return CaptureFormatInfo(
            index: index,
            width: Int(dimensions.width),
            height: Int(dimensions.height),
            minFrameRate: ranges.map(\.minFrameRate).min() ?? 0,
            maxFrameRate: ranges.map(\.maxFrameRate).max() ?? 0,
            isVideoRange420: CMFormatDescriptionGetMediaSubType(format.formatDescription)
                == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        )
    }

    /// A frame rate the format accepts: setting an unsupported duration raises an exception.
    private static func supportedRate(_ fps: Int, in format: AVCaptureDevice.Format) -> Int {
        let ranges = format.videoSupportedFrameRateRanges
        let wanted = Double(fps)
        if ranges.contains(where: { $0.minFrameRate <= wanted && wanted <= $0.maxFrameRate }) { return fps }
        let maximum = ranges.map(\.maxFrameRate).max() ?? wanted
        let minimum = ranges.map(\.minFrameRate).min() ?? wanted
        return max(1, Int(min(max(wanted, minimum.rounded(.up)), maximum.rounded(.down))))
    }

    private static func format(width: Int, height: Int, fps: Int, rotation: CGFloat, mirrored: Bool,
                               camera: CameraSelection) -> VideoCaptureFormat {
        let degrees = Int(normalize(rotation))
        let portrait = degrees % 180 != 0
        return VideoCaptureFormat(width: portrait ? height : width, height: portrait ? width : height, fps: fps,
                                  rotationDegrees: degrees, mirrored: mirrored, camera: camera)
    }

    private static func normalize(_ angle: CGFloat) -> CGFloat {
        let snapped = (angle / 90).rounded() * 90
        let wrapped = snapped.truncatingRemainder(dividingBy: 360)
        return wrapped < 0 ? wrapped + 360 : wrapped
    }

    static func interruptionDescription(_ rawValue: Int?) -> String {
        guard let rawValue, let reason = AVCaptureSession.InterruptionReason(rawValue: rawValue) else { return "unknown" }
        switch reason {
        case .videoDeviceNotAvailableInBackground: return "background"
        case .audioDeviceInUseByAnotherClient: return "audioDeviceInUseByAnotherClient"
        case .videoDeviceInUseByAnotherClient: return "inUseByAnotherClient"
        case .videoDeviceNotAvailableWithMultipleForegroundApps: return "multipleForegroundApps"
        case .videoDeviceNotAvailableDueToSystemPressure: return "systemPressure"
        @unknown default: return "other(\(rawValue))"
        }
    }

    /// Lines for the boot report: every camera and its distinct 420v sizes.
    static func availableCamerasSummary() -> [String] {
        let discovery = AVCaptureDevice.DiscoverySession(
            deviceTypes: [.builtInWideAngleCamera, .builtInUltraWideCamera, .builtInTelephotoCamera],
            mediaType: .video,
            position: .unspecified
        )
        return discovery.devices.map { device in
            let sizes = device.formats
                .map { describe($0, index: 0) }
                .filter(\.isVideoRange420)
                .map { "\($0.width)x\($0.height)@\(Int($0.maxFrameRate))" }
            let unique = Array(Set(sizes)).sorted()
            return "camera \(device.localizedName): \(unique.joined(separator: ", "))"
        }
    }
}

extension ThermalLevel {
    init(_ level: AVCaptureDevice.SystemPressureState.Level) {
        switch level {
        case .nominal: self = .nominal
        case .fair: self = .fair
        case .serious: self = .serious
        case .critical: self = .critical
        case .shutdown: self = .shutdown
        default: self = .nominal
        }
    }
}

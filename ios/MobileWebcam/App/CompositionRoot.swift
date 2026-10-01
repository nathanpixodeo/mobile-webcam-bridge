import AVFoundation
import BridgeKit
import Foundation

/// Builds the object graph once and owns it for the app's lifetime. This is the only place that
/// knows concrete types; everything else receives its collaborators through initialisers.
///
/// Threads: one serial queue each for transport (Network callbacks and the session), video
/// (capture and encoding) and audio. UI state lives on the main actor.
@MainActor
final class CompositionRoot: AppActions {
    static let companionPort: UInt16 = 27_100

    let model: AppModel
    private let logger: AppLogger
    private let identity: AppIdentity
    private let coordinator: SessionCoordinator
    private let server: CompanionServer
    private let videoController: VideoStreamController
    private let audioController: AudioStreamController
    private let systemMonitor: SystemStatusMonitor
    private let thermalMonitor: ThermalMonitor
    private let lifecycleMonitor: LifecycleMonitor
    private let idleTimer: IdleTimerController
    private let metricKit: MetricKitCollector
    private var isStarted = false

    init() {
        let clock = HostTimeClock()
        let logger = AppLogger(subsystem: Bundle.main.bundleIdentifier ?? "MobileWebcam", clock: clock)
        let identity = AppIdentity.current()
        let transportQueue = DispatchQueue(label: "bridge.transport", qos: .userInteractive)
        let videoQueue = DispatchQueue(label: "bridge.video", qos: .userInteractive)
        let audioQueue = DispatchQueue(label: "bridge.audio", qos: .userInteractive)
        let model = AppModel(identity: identity, port: Self.companionPort)
        let idleTimer = IdleTimerController()
        let metricKit = MetricKitCollector(logger: logger)

        let coordinator = SessionCoordinator(
            queue: transportQueue,
            identity: identity,
            clock: clock,
            logger: logger,
            diagnostics: metricKit,
            bootReport: CameraVideoSource.availableCamerasSummary(),
            onSnapshot: { snapshot in
                Task { @MainActor in
                    model.apply(snapshot)
                    idleTimer.setKeepAwake(snapshot.connection == .connected)
                }
            }
        )

        let videoController = VideoStreamController(
            queue: videoQueue,
            camera: CameraVideoSource(queue: videoQueue, logger: logger),
            synthetic: SyntheticVideoSource(queue: videoQueue, clock: clock),
            sink: coordinator,
            logger: logger
        )
        let audioController = AudioStreamController(
            queue: audioQueue,
            microphone: MicAudioSource(queue: audioQueue, audioSession: AudioSessionController(logger: logger), logger: logger),
            synthetic: SyntheticAudioSource(queue: audioQueue, clock: clock),
            sink: coordinator,
            logger: logger
        )
        coordinator.bind(media: MediaController(video: videoController, audio: audioController))

        let server = CompanionServer(
            port: Self.companionPort,
            queue: transportQueue,
            logger: logger,
            onConnection: { connection in coordinator.accept(connection) },
            onStateChange: { state in coordinator.serverStateChanged(state) }
        )

        let systemMonitor = SystemStatusMonitor { snapshot in
            coordinator.updateSystem(snapshot)
            model.apply(snapshot)
        }
        let thermalMonitor = ThermalMonitor { level in
            videoController.setProcessThermalLevel(level)
            systemMonitor.update(thermal: level)
        }
        let lifecycleMonitor = LifecycleMonitor { state in
            systemMonitor.update(appState: state)
        }

        self.model = model
        self.logger = logger
        self.identity = identity
        self.coordinator = coordinator
        self.server = server
        self.videoController = videoController
        self.audioController = audioController
        self.systemMonitor = systemMonitor
        self.thermalMonitor = thermalMonitor
        self.lifecycleMonitor = lifecycleMonitor
        self.idleTimer = idleTimer
        self.metricKit = metricKit
        model.actions = self
    }

    func start() async {
        guard !isStarted else { return }
        isStarted = true
        let app = identity.app
        logger.info("app", "Mobile Webcam \(app.version) (\(app.build), \(app.gitSha)) on \(identity.device.model)")

        metricKit.register()
        lifecycleMonitor.start()
        thermalMonitor.start()
        systemMonitor.start()
        coordinator.start()
        server.start()
        await requestPermissions()
    }

    // MARK: - AppActions

    func setTestPatternEnabled(_ enabled: Bool) {
        logger.info("app", "Test pattern \(enabled ? "on" : "off")")
        videoController.setSyntheticSource(enabled)
        audioController.setSyntheticSource(enabled)
    }

    func requestPermissions() async {
        if AVCaptureDevice.authorizationStatus(for: .video) == .notDetermined {
            _ = await AVCaptureDevice.requestAccess(for: .video)
        }
        if AVCaptureDevice.authorizationStatus(for: .audio) == .notDetermined {
            _ = await AVCaptureDevice.requestAccess(for: .audio)
        }
        systemMonitor.refreshPermissions()
    }

    func recentLogs() -> [LogEntry] {
        logger.recentEntries(limit: 200)
    }
}

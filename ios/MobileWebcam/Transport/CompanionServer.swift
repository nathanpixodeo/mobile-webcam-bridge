import Foundation
import Network

/// Listens for the host on the device loopback interface. usbmux opens host connections to
/// `127.0.0.1:<port>` on the phone, so nothing is ever exposed on Wi-Fi or cellular.
final class CompanionServer: @unchecked Sendable {
    enum State: Equatable, Sendable {
        case stopped
        case starting
        case listening(port: UInt16)
        case failed(String)
    }

    /// Ways to bind to loopback, tried in order. `requiredLocalEndpoint` is the documented way
    /// to choose the local address of a listener; restricting the interface type is a fallback
    /// in case a future OS rejects the first form.
    private enum BindStrategy: CaseIterable, CustomStringConvertible {
        case loopbackEndpoint
        case loopbackInterface

        var description: String {
            switch self {
            case .loopbackEndpoint: "127.0.0.1 endpoint"
            case .loopbackInterface: "loopback interface"
            }
        }
    }

    private let port: NWEndpoint.Port
    private let queue: DispatchQueue
    private let logger: AppLogger
    private let onConnection: @Sendable (NWConnection) -> Void
    private let onStateChange: @Sendable (State) -> Void
    private var listener: NWListener?
    private var strategyIndex = 0
    private var isRunning = false

    init(port: UInt16, queue: DispatchQueue, logger: AppLogger,
         onConnection: @escaping @Sendable (NWConnection) -> Void,
         onStateChange: @escaping @Sendable (State) -> Void) {
        self.port = NWEndpoint.Port(rawValue: port) ?? NWEndpoint.Port(integerLiteral: 27_100)
        self.queue = queue
        self.logger = logger
        self.onConnection = onConnection
        self.onStateChange = onStateChange
    }

    func start() {
        queue.async { [self] in
            guard !isRunning else { return }
            isRunning = true
            strategyIndex = 0
            startListener()
        }
    }

    func stop() {
        queue.async { [self] in
            isRunning = false
            listener?.cancel()
            listener = nil
            onStateChange(.stopped)
        }
    }

    private func startListener() {
        let strategy = BindStrategy.allCases[strategyIndex]
        do {
            let listener = try makeListener(strategy)
            listener.newConnectionHandler = { [weak self] connection in self?.onConnection(connection) }
            listener.stateUpdateHandler = { [weak self] state in self?.handle(state, strategy: strategy) }
            self.listener = listener
            onStateChange(.starting)
            listener.start(queue: queue)
        } catch {
            failed(strategy, message: "\(error)")
        }
    }

    private func makeListener(_ strategy: BindStrategy) throws -> NWListener {
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        let parameters = NWParameters(tls: nil, tcp: tcp)
        parameters.allowLocalEndpointReuse = true
        parameters.includePeerToPeer = false
        switch strategy {
        case .loopbackEndpoint:
            parameters.requiredLocalEndpoint = .hostPort(host: .ipv4(.loopback), port: port)
            return try NWListener(using: parameters)
        case .loopbackInterface:
            parameters.requiredInterfaceType = .loopback
            return try NWListener(using: parameters, on: port)
        }
    }

    private func handle(_ state: NWListener.State, strategy: BindStrategy) {
        switch state {
        case .ready:
            let boundPort = listener?.port?.rawValue ?? port.rawValue
            logger.info("transport", "Listening on port \(boundPort) (\(strategy))")
            onStateChange(.listening(port: boundPort))
        case .failed(let error):
            failed(strategy, message: "\(error)")
        case .waiting(let error):
            logger.warn("transport", "Listener waiting (\(strategy)): \(error)")
        default:
            break
        }
    }

    private func failed(_ strategy: BindStrategy, message: String) {
        logger.error("transport", "Listener failed (\(strategy)): \(message)")
        listener?.cancel()
        listener = nil
        onStateChange(.failed(message))
        guard isRunning else { return }
        strategyIndex = (strategyIndex + 1) % BindStrategy.allCases.count
        // Try the next strategy quickly; after a full round, back off.
        let delay: DispatchTimeInterval = strategyIndex == 0 ? .seconds(5) : .milliseconds(500)
        queue.asyncAfter(deadline: .now() + delay) { [weak self] in
            guard let self, self.isRunning, self.listener == nil else { return }
            self.startListener()
        }
    }
}

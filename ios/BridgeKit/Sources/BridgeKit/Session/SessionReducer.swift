/// Identifies one accepted TCP connection; a new connection replaces the previous one.
public struct ConnectionID: Hashable, Sendable, CustomStringConvertible {
    public let rawValue: UInt64

    public init(_ rawValue: UInt64) {
        self.rawValue = rawValue
    }

    public var description: String { "#\(rawValue)" }
}

public enum CloseReason: Equatable, Sendable {
    case replaced
    case versionMismatch
    case badHandshake(String)
    case heartbeatTimeout
    case protocolViolation(String)
    case peerError(ErrorCode)
    case audioBacklog
}

/// Device-side session state. Pure value: all side effects are described by `SessionEffect`.
public struct SessionState: Equatable, Sendable {
    public enum Phase: Equatable, Sendable {
        /// No host connection.
        case idle
        /// Connected; our Hello is sent, the host's has not arrived yet.
        case awaitingHello(ConnectionID)
        /// Handshake complete.
        case ready(ConnectionID, effectiveMinor: Int)
    }

    public var phase: Phase = .idle
    /// Video the host asked for (nil = stopped).
    public var video: StartVideoMessage?
    /// Audio the host asked for (nil = stopped).
    public var audio: StartAudioMessage?

    public init() {}

    public var connection: ConnectionID? {
        switch phase {
        case .idle: nil
        case .awaitingHello(let id), .ready(let id, _): id
        }
    }

    public var readyConnection: ConnectionID? {
        if case .ready(let id, _) = phase { return id }
        return nil
    }
}

public enum SessionEvent: Equatable, Sendable {
    case connectionOpened(ConnectionID)
    case connectionClosed(ConnectionID)
    case commandReceived(ConnectionID, HostCommand, receivedAtUs: Int64)
    case commandRejected(ConnectionID, MessageDecodingError)
    case protocolViolation(ConnectionID, ProtocolError)
    case heartbeatTimedOut(ConnectionID)
    case audioBacklogExceeded(ConnectionID)
    case videoFailed(ErrorMessage)
    case audioFailed(ErrorMessage)
}

public enum SessionEffect: Equatable, Sendable {
    case sendHello(ConnectionID)
    case sendError(ConnectionID, ErrorMessage)
    case sendPong(ConnectionID, PongPayload)
    case recordClockSample(t0: Int64, t1: Int64, t2: Int64, t3: Int64)
    case handshakeCompleted(ConnectionID, effectiveMinor: Int)
    case closeConnection(ConnectionID, CloseReason)
    case startVideo(StartVideoMessage)
    case stopVideo
    case requestKeyframe
    case startAudio(StartAudioMessage)
    case stopAudio
}

public struct SessionTransition: Equatable, Sendable {
    public var state: SessionState
    public var effects: [SessionEffect]
}

/// The device session state machine (SPEC §2). Media streams only run while a handshaken host is
/// connected; losing the connection stops them, and the host replays its desired streams after
/// reconnecting.
public enum SessionReducer {
    public static let localVersion = ProtocolVersion.current

    public static func reduce(_ state: SessionState, _ event: SessionEvent) -> SessionTransition {
        var builder = Builder(state: state)
        builder.apply(event)
        return SessionTransition(state: builder.state, effects: builder.effects)
    }

    private struct Builder {
        var state: SessionState
        var effects: [SessionEffect] = []

        mutating func apply(_ event: SessionEvent) {
            switch event {
            case .connectionOpened(let id):
                if let current = state.connection, current != id {
                    effects.append(.closeConnection(current, .replaced))
                }
                stopStreams()
                state.phase = .awaitingHello(id)
                effects.append(.sendHello(id))

            case .connectionClosed(let id):
                guard state.connection == id else { return }
                stopStreams()
                state.phase = .idle

            case .heartbeatTimedOut(let id):
                guard state.connection == id else { return }
                tearDown(id, reason: .heartbeatTimeout)

            case .audioBacklogExceeded(let id):
                guard state.connection == id else { return }
                tearDown(id, reason: .audioBacklog)

            case .protocolViolation(let id, let error):
                guard state.connection == id else { return }
                effects.append(.sendError(id, ErrorMessage(code: .badRequest, message: "Protocol violation: \(error.code)", fatal: true)))
                tearDown(id, reason: .protocolViolation(error.code))

            case .commandRejected(let id, let error):
                guard state.connection == id else { return }
                rejected(error, on: id)

            case .commandReceived(let id, let command, let receivedAtUs):
                guard state.connection == id else { return }
                switch state.phase {
                case .idle: return
                case .awaitingHello: beforeHello(command, on: id)
                case .ready: whileReady(command, on: id, receivedAtUs: receivedAtUs)
                }

            case .videoFailed(let error):
                state.video = nil
                if let id = state.readyConnection { effects.append(.sendError(id, error)) }

            case .audioFailed(let error):
                state.audio = nil
                if let id = state.readyConnection { effects.append(.sendError(id, error)) }
            }
        }

        private mutating func beforeHello(_ command: HostCommand, on id: ConnectionID) {
            switch command {
            case .hello(let hello):
                guard hello.protocolVersion.major == SessionReducer.localVersion.major else {
                    let message = "Unsupported protocol major version \(hello.protocolVersion.major)"
                    effects.append(.sendError(id, ErrorMessage(code: .versionMismatch, message: message, fatal: true)))
                    tearDown(id, reason: .versionMismatch)
                    return
                }
                guard hello.role == .host else {
                    effects.append(.sendError(id, ErrorMessage(code: .badRequest, message: "Expected role host", fatal: true)))
                    tearDown(id, reason: .badHandshake("peer role is \(hello.role.rawValue)"))
                    return
                }
                let minor = min(SessionReducer.localVersion.minor, hello.protocolVersion.minor)
                state.phase = .ready(id, effectiveMinor: minor)
                effects.append(.handshakeCompleted(id, effectiveMinor: minor))
            case .error(let error) where error.fatal:
                tearDown(id, reason: .peerError(error.code))
            default:
                // SPEC §2.3: everything else before the host Hello is ignored.
                return
            }
        }

        private mutating func whileReady(_ command: HostCommand, on id: ConnectionID, receivedAtUs: Int64) {
            switch command {
            case .hello, .ignored:
                return
            case .error(let error):
                if error.fatal { tearDown(id, reason: .peerError(error.code)) }
            case .ping(let t0):
                effects.append(.sendPong(id, PongPayload(echoT0: t0, t1: receivedAtUs)))
            case .pong(let payload, let sentAtUs):
                effects.append(.recordClockSample(t0: payload.echoT0, t1: payload.t1, t2: sentAtUs, t3: receivedAtUs))
            case .startVideo(let request):
                guard state.video != request else { return }
                state.video = request
                effects.append(.startVideo(request))
            case .stopVideo:
                guard state.video != nil else { return }
                state.video = nil
                effects.append(.stopVideo)
            case .requestKeyframe:
                if state.video != nil { effects.append(.requestKeyframe) }
            case .startAudio(let request):
                guard state.audio != request else { return }
                state.audio = request
                effects.append(.startAudio(request))
            case .stopAudio:
                guard state.audio != nil else { return }
                state.audio = nil
                effects.append(.stopAudio)
            }
        }

        private mutating func rejected(_ error: MessageDecodingError, on id: ConnectionID) {
            switch state.phase {
            case .idle:
                return
            case .awaitingHello:
                guard error.type == .hello else { return }
                effects.append(.sendError(id, ErrorMessage(code: .badRequest, message: "Invalid Hello: \(error.detail)", fatal: true)))
                tearDown(id, reason: .badHandshake(error.detail))
            case .ready:
                let message = "Invalid \(error.type): \(error.detail)"
                effects.append(.sendError(id, ErrorMessage(code: .badRequest, message: message, fatal: false)))
            }
        }

        private mutating func tearDown(_ id: ConnectionID, reason: CloseReason) {
            stopStreams()
            state.phase = .idle
            effects.append(.closeConnection(id, reason))
        }

        private mutating func stopStreams() {
            if state.video != nil {
                state.video = nil
                effects.append(.stopVideo)
            }
            if state.audio != nil {
                state.audio = nil
                effects.append(.stopAudio)
            }
        }
    }
}

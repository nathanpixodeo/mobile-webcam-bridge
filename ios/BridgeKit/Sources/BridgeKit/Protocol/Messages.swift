import Foundation

// JSON payloads of SPEC §4. Every type is a plain Codable value; `CanonicalJSON` produces the
// wire form, `JSONDecoder` reads it (unknown keys are ignored, as the spec requires).

public struct ProtocolVersion: Codable, Equatable, Sendable {
    public var major: Int
    public var minor: Int

    public init(major: Int, minor: Int) {
        self.major = major
        self.minor = minor
    }

    public static let current = ProtocolVersion(major: 1, minor: 0)
}

public enum PeerRole: String, Codable, Sendable {
    case host
    case device
}

public struct AppDescriptor: Codable, Equatable, Sendable {
    public var name: String
    public var version: String
    public var build: String
    public var gitSha: String

    public init(name: String, version: String, build: String, gitSha: String) {
        self.name = name
        self.version = version
        self.build = build
        self.gitSha = gitSha
    }
}

public struct DeviceDescriptor: Codable, Equatable, Sendable {
    public var model: String
    public var name: String
    public var os: String

    public init(model: String, name: String, os: String) {
        self.model = model
        self.name = name
        self.os = os
    }
}

public struct HelloMessage: Codable, Equatable, Sendable {
    public var protocolVersion: ProtocolVersion
    public var role: PeerRole
    public var app: AppDescriptor
    public var device: DeviceDescriptor?
    public var features: [String]

    public init(protocolVersion: ProtocolVersion = .current, role: PeerRole, app: AppDescriptor,
                device: DeviceDescriptor? = nil, features: [String]) {
        self.protocolVersion = protocolVersion
        self.role = role
        self.app = app
        self.device = device
        self.features = features.sorted()
    }

    enum CodingKeys: String, CodingKey {
        case protocolVersion = "protocol"
        case role
        case app
        case device
        case features
    }

    public init(from decoder: any Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        protocolVersion = try container.decode(ProtocolVersion.self, forKey: .protocolVersion)
        role = try container.decode(PeerRole.self, forKey: .role)
        app = try container.decode(AppDescriptor.self, forKey: .app)
        device = try container.decodeIfPresent(DeviceDescriptor.self, forKey: .device)
        features = try container.decodeIfPresent([String].self, forKey: .features) ?? []
    }
}

/// Extensible error code: unknown codes from a newer peer still decode.
public struct ErrorCode: RawRepresentable, Codable, Hashable, Sendable {
    public let rawValue: String

    public init(rawValue: String) {
        self.rawValue = rawValue
    }

    public static let versionMismatch = ErrorCode(rawValue: "VERSION_MISMATCH")
    public static let badRequest = ErrorCode(rawValue: "BAD_REQUEST")
    public static let permissionDenied = ErrorCode(rawValue: "PERMISSION_DENIED")
    public static let cameraUnavailable = ErrorCode(rawValue: "CAMERA_UNAVAILABLE")
    public static let micUnavailable = ErrorCode(rawValue: "MIC_UNAVAILABLE")
    public static let encoderFailure = ErrorCode(rawValue: "ENCODER_FAILURE")
    public static let internalError = ErrorCode(rawValue: "INTERNAL")
}

public struct ErrorMessage: Codable, Equatable, Sendable {
    public var code: ErrorCode
    public var message: String
    public var fatal: Bool

    public init(code: ErrorCode, message: String, fatal: Bool) {
        self.code = code
        self.message = message
        self.fatal = fatal
    }
}

public enum CameraSelection: String, Codable, CaseIterable, Sendable {
    case backWide = "back.wide"
    case backUltraWide = "back.ultraWide"
    case backTelephoto = "back.telephoto"
    case front
}

public enum OrientationMode: String, Codable, Sendable {
    case auto
    case landscape
    case portrait
}

public enum EncoderMode: String, Codable, Sendable {
    case lowLatency
    case standard
}

public struct StartVideoMessage: Codable, Equatable, Sendable {
    public var width: Int
    public var height: Int
    public var fps: Int
    public var bitrateKbps: Int
    public var camera: CameraSelection
    public var mirror: Bool
    public var orientation: OrientationMode
    public var encoder: EncoderMode

    public init(width: Int, height: Int, fps: Int, bitrateKbps: Int, camera: CameraSelection,
                mirror: Bool, orientation: OrientationMode, encoder: EncoderMode) {
        self.width = width
        self.height = height
        self.fps = fps
        self.bitrateKbps = bitrateKbps
        self.camera = camera
        self.mirror = mirror
        self.orientation = orientation
        self.encoder = encoder
    }

    /// Range checks from SPEC §4 `StartVideo`.
    public func validate() throws {
        guard (160...3840).contains(width), (120...3840).contains(height) else {
            throw MessageDecodingError.invalidValue(type: .startVideo, detail: "size \(width)x\(height) out of range")
        }
        guard (15...60).contains(fps) else {
            throw MessageDecodingError.invalidValue(type: .startVideo, detail: "fps \(fps) out of range 15-60")
        }
        guard (500...40_000).contains(bitrateKbps) else {
            throw MessageDecodingError.invalidValue(type: .startVideo, detail: "bitrate \(bitrateKbps) out of range 500-40000")
        }
    }
}

public enum AudioProcessing: String, Codable, Sendable {
    case standard
    case raw
    case voice
}

public struct StartAudioMessage: Codable, Equatable, Sendable {
    public var processing: AudioProcessing

    public init(processing: AudioProcessing) {
        self.processing = processing
    }
}

public struct VideoConfigMessage: Codable, Equatable, Sendable {
    public var configId: Int
    public var codec: String
    public var profile: String
    public var width: Int
    public var height: Int
    public var fps: Int
    public var bitrateKbps: Int
    public var rotationDeg: Int
    public var mirrored: Bool
    public var encoder: EncoderMode
    public var camera: CameraSelection

    public init(configId: Int, codec: String = "h264", profile: String = "constrainedHigh", width: Int,
                height: Int, fps: Int, bitrateKbps: Int, rotationDeg: Int, mirrored: Bool,
                encoder: EncoderMode, camera: CameraSelection) {
        self.configId = configId
        self.codec = codec
        self.profile = profile
        self.width = width
        self.height = height
        self.fps = fps
        self.bitrateKbps = bitrateKbps
        self.rotationDeg = rotationDeg
        self.mirrored = mirrored
        self.encoder = encoder
        self.camera = camera
    }
}

public struct AudioConfigMessage: Codable, Equatable, Sendable {
    public var configId: Int
    public var format: String
    public var sampleRate: Int
    public var channels: Int

    public init(configId: Int, format: String = "s16le", sampleRate: Int = 48_000, channels: Int = 1) {
        self.configId = configId
        self.format = format
        self.sampleRate = sampleRate
        self.channels = channels
    }
}

public enum StreamState: String, Codable, Sendable {
    case off
    case starting
    case running
    case interrupted
    case error
}

public struct VideoStreamStatus: Codable, Equatable, Sendable {
    public var state: StreamState
    public var reason: String?
    public var width: Int?
    public var height: Int?
    public var fps: Int?
    public var bitrateKbps: Int?

    public init(state: StreamState, reason: String? = nil, width: Int? = nil, height: Int? = nil,
                fps: Int? = nil, bitrateKbps: Int? = nil) {
        self.state = state
        self.reason = reason
        self.width = width
        self.height = height
        self.fps = fps
        self.bitrateKbps = bitrateKbps
    }

    public static let off = VideoStreamStatus(state: .off)
}

public struct AudioStreamStatus: Codable, Equatable, Sendable {
    public var state: StreamState
    public var reason: String?

    public init(state: StreamState, reason: String? = nil) {
        self.state = state
        self.reason = reason
    }

    public static let off = AudioStreamStatus(state: .off)
}

public struct BatteryStatus: Codable, Equatable, Sendable {
    public var levelPercent: Int
    public var charging: Bool

    public init(levelPercent: Int, charging: Bool) {
        self.levelPercent = levelPercent
        self.charging = charging
    }
}

public enum PermissionState: String, Codable, Sendable {
    case authorized
    case denied
    case restricted
    case notDetermined
}

public struct PermissionsStatus: Codable, Equatable, Sendable {
    public var camera: PermissionState
    public var microphone: PermissionState

    public init(camera: PermissionState, microphone: PermissionState) {
        self.camera = camera
        self.microphone = microphone
    }
}

public enum ThermalStateName: String, Codable, Sendable {
    case nominal
    case fair
    case serious
    case critical
}

public enum AppState: String, Codable, Sendable {
    case active
    case inactive
    case background
}

public struct StatusMessage: Codable, Equatable, Sendable {
    public var appState: AppState
    public var audio: AudioStreamStatus
    public var battery: BatteryStatus?
    public var lowPower: Bool
    public var permissions: PermissionsStatus
    public var thermal: ThermalStateName
    public var video: VideoStreamStatus

    public init(appState: AppState, audio: AudioStreamStatus, battery: BatteryStatus?, lowPower: Bool,
                permissions: PermissionsStatus, thermal: ThermalStateName, video: VideoStreamStatus) {
        self.appState = appState
        self.audio = audio
        self.battery = battery
        self.lowPower = lowPower
        self.permissions = permissions
        self.thermal = thermal
        self.video = video
    }
}

public enum LogLevel: String, Codable, CaseIterable, Sendable, Comparable {
    case debug
    case info
    case warn
    case error

    private var rank: Int {
        switch self {
        case .debug: 0
        case .info: 1
        case .warn: 2
        case .error: 3
        }
    }

    public static func < (lhs: LogLevel, rhs: LogLevel) -> Bool { lhs.rank < rhs.rank }
}

public struct LogMessage: Codable, Equatable, Sendable {
    public var level: LogLevel
    public var category: String
    public var message: String

    public init(level: LogLevel, category: String, message: String) {
        self.level = level
        self.category = category
        self.message = message
    }

    /// Cuts `message` (on a character boundary) so the encoded payload stays well under the
    /// 8 KiB `Log` limit even after JSON escaping.
    public func truncated(maxMessageBytes: Int = 6_000) -> LogMessage {
        guard message.utf8.count > maxMessageBytes else { return self }
        var result = ""
        var bytes = 0
        for character in message {
            let size = character.utf8.count
            if bytes + size > maxMessageBytes { break }
            result.append(character)
            bytes += size
        }
        return LogMessage(level: level, category: category, message: result + "…")
    }
}

/// `Pong` binary payload: `echoT0` and `t1`, big-endian i64 each (SPEC §3.1).
public struct PongPayload: Equatable, Sendable {
    public var echoT0: Int64
    public var t1: Int64

    public init(echoT0: Int64, t1: Int64) {
        self.echoT0 = echoT0
        self.t1 = t1
    }

    public init(decoding payload: Data) throws {
        var reader = ByteReader(payload)
        guard payload.count == Int(ProtocolLimits.pongPayloadLength),
              let echoT0 = reader.readInt64(),
              let t1 = reader.readInt64()
        else {
            throw ProtocolError.badPayloadLength(rawType: PacketType.pong.rawValue, length: UInt32(clamping: payload.count))
        }
        self.init(echoT0: echoT0, t1: t1)
    }

    public func encoded() -> Data {
        var writer = ByteWriter(capacity: Int(ProtocolLimits.pongPayloadLength))
        writer.write(bigEndian: echoT0)
        writer.write(bigEndian: t1)
        return writer.data
    }
}

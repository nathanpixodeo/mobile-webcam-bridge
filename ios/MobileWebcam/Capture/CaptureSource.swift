import BridgeKit
import CoreVideo
import Foundation

/// What the session asks a video source for. Sizes are in landscape terms (SPEC §4).
struct VideoCaptureRequest: Equatable, Sendable {
    var width: Int
    var height: Int
    var fps: Int
    var camera: CameraSelection
    var mirror: Bool
    var orientation: OrientationMode

    init(_ message: StartVideoMessage, fpsCap: Int?) {
        width = max(message.width, message.height)
        height = min(message.width, message.height)
        fps = fpsCap.map { min(message.fps, $0) } ?? message.fps
        camera = message.camera
        mirror = message.mirror
        orientation = message.orientation
    }
}

/// What a video source actually delivers: frame size after rotation, effective rate, camera used.
struct VideoCaptureFormat: Equatable, Sendable {
    var width: Int
    var height: Int
    var fps: Int
    var rotationDegrees: Int
    var mirrored: Bool
    var camera: CameraSelection
}

enum CaptureError: Error, Equatable, Sendable {
    case permissionDenied(String)
    case deviceUnavailable(String)
    case configurationFailed(String)
    case encoderFailed(Int32)

    var errorMessage: ErrorMessage {
        switch self {
        case .permissionDenied(let detail):
            ErrorMessage(code: .permissionDenied, message: detail, fatal: false)
        case .deviceUnavailable(let detail), .configurationFailed(let detail):
            ErrorMessage(code: .cameraUnavailable, message: detail, fatal: false)
        case .encoderFailed(let status):
            ErrorMessage(code: .encoderFailure, message: "VideoToolbox error \(status)", fatal: false)
        }
    }
}

enum SourceEvent: Equatable, Sendable {
    case interrupted(reason: String)
    case resumed
    case failed(String)
}

/// Produces raw frames. All calls, and all delegate callbacks, happen on the video queue.
protocol VideoFrameSource: AnyObject {
    func start(_ request: VideoCaptureRequest, delegate: any VideoFrameSourceDelegate) throws -> VideoCaptureFormat
    func stop()
    func applyFrameRateCap(_ fps: Int)
}

protocol VideoFrameSourceDelegate: AnyObject {
    func frameSource(didOutput pixelBuffer: CVPixelBuffer, presentationTimeUs: Int64)
    func frameSource(didChangeFormat format: VideoCaptureFormat)
    func frameSource(didEmit event: SourceEvent)
    func frameSource(didChangePressure level: ThermalLevel)
}

/// Produces 48 kHz mono `s16le`. All calls, and all delegate callbacks, happen on the audio queue.
protocol AudioSampleSource: AnyObject {
    func start(_ request: StartAudioMessage, delegate: any AudioSampleSourceDelegate) throws
    func stop()
}

protocol AudioSampleSourceDelegate: AnyObject {
    func audioSource(didCapture pcm: Data, timestampUs: Int64, discontinuity: Bool)
    func audioSource(didEmit event: SourceEvent)
}

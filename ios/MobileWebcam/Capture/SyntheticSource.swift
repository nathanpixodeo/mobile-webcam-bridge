import BridgeKit
import CoreVideo
import Foundation

/// Test pattern for debugging without a camera (and in the simulator): a moving bar plus a
/// binary frame counter, so dropped or repeated frames are visible on the host.
final class SyntheticVideoSource: VideoFrameSource, @unchecked Sendable {
    private let queue: DispatchQueue
    private let clock: any MonotonicClock
    private weak var delegate: (any VideoFrameSourceDelegate)?
    private var timer: (any DispatchSourceTimer)?
    private var pool: CVPixelBufferPool?
    private var format: VideoCaptureFormat?
    private var frameIndex: UInt32 = 0

    init(queue: DispatchQueue, clock: any MonotonicClock) {
        self.queue = queue
        self.clock = clock
    }

    func start(_ request: VideoCaptureRequest, delegate: any VideoFrameSourceDelegate) throws -> VideoCaptureFormat {
        dispatchPrecondition(condition: .onQueue(queue))
        stop()
        let width = request.width & ~1
        let height = request.height & ~1
        guard let pool = Self.makePool(width: width, height: height) else {
            throw CaptureError.configurationFailed("Cannot create pixel buffer pool")
        }
        let format = VideoCaptureFormat(width: width, height: height, fps: request.fps, rotationDegrees: 0,
                                        mirrored: false, camera: request.camera)
        self.pool = pool
        self.format = format
        self.delegate = delegate
        frameIndex = 0
        schedule(fps: request.fps)
        return format
    }

    func stop() {
        timer?.cancel()
        timer = nil
        pool = nil
        delegate = nil
    }

    func applyFrameRateCap(_ fps: Int) {
        guard timer != nil else { return }
        format?.fps = fps
        schedule(fps: fps)
    }

    private func schedule(fps: Int) {
        timer?.cancel()
        let timer = DispatchSource.makeTimerSource(queue: queue)
        let interval = DispatchTimeInterval.nanoseconds(1_000_000_000 / max(1, fps))
        timer.schedule(deadline: .now(), repeating: interval, leeway: .milliseconds(1))
        timer.setEventHandler { [weak self] in self?.emitFrame() }
        timer.resume()
        self.timer = timer
    }

    private func emitFrame() {
        guard let pool, let delegate else { return }
        var pixelBuffer: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixelBuffer) == kCVReturnSuccess,
              let pixelBuffer else { return }
        TestPatternRenderer.render(into: pixelBuffer, frameIndex: frameIndex)
        frameIndex &+= 1
        delegate.frameSource(didOutput: pixelBuffer, presentationTimeUs: clock.nowMicroseconds())
    }

    private static func makePool(width: Int, height: Int) -> CVPixelBufferPool? {
        let attributes: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            kCVPixelBufferWidthKey as String: width,
            kCVPixelBufferHeightKey as String: height,
            kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any](),
        ]
        var pool: CVPixelBufferPool?
        CVPixelBufferPoolCreate(kCFAllocatorDefault, nil, attributes as CFDictionary, &pool)
        return pool
    }
}

/// Draws the test pattern into a 420v (NV12, video range) pixel buffer.
enum TestPatternRenderer {
    static func render(into buffer: CVPixelBuffer, frameIndex: UInt32) {
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        guard let lumaBase = CVPixelBufferGetBaseAddressOfPlane(buffer, 0),
              let chromaBase = CVPixelBufferGetBaseAddressOfPlane(buffer, 1) else { return }

        let width = CVPixelBufferGetWidthOfPlane(buffer, 0)
        let height = CVPixelBufferGetHeightOfPlane(buffer, 0)
        let lumaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0)
        let chromaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1)
        let chromaHeight = CVPixelBufferGetHeightOfPlane(buffer, 1)
        let luma = lumaBase.assumingMemoryBound(to: UInt8.self)
        let chroma = chromaBase.assumingMemoryBound(to: UInt8.self)

        // Dark grey background, neutral chroma.
        for row in 0..<height { memset(luma + row * lumaStride, 40, width) }
        for row in 0..<chromaHeight { memset(chroma + row * chromaStride, 128, width) }

        // A white bar crossing the frame once every two seconds at 30 fps.
        let barWidth = max(8, width / 40)
        let travel = max(1, width - barWidth)
        let barX = Int(frameIndex % 60) * travel / 59
        for row in 0..<height { memset(luma + row * lumaStride + barX, 235, barWidth) }

        // 16-bit binary frame counter along the top edge: white = 1, black = 0.
        let cell = max(8, width / 32)
        for bit in 0..<16 {
            let value: Int32 = (frameIndex >> UInt32(15 - bit)) & 1 == 1 ? 235 : 16
            let x = bit * cell
            guard x + cell <= width else { break }
            for row in 0..<min(cell, height) { memset(luma + row * lumaStride + x, value, cell) }
        }
    }
}

/// 440 Hz tone at -12 dBFS, paced by the clock so the long-term rate is exactly 48 kHz.
final class SyntheticAudioSource: AudioSampleSource, @unchecked Sendable {
    private static let tickMilliseconds = 20

    private let queue: DispatchQueue
    private let clock: any MonotonicClock
    private weak var delegate: (any AudioSampleSourceDelegate)?
    private var timer: (any DispatchSourceTimer)?
    private var generator = ToneGenerator()
    private var startUs: Int64 = 0
    private var producedSamples: Int64 = 0

    init(queue: DispatchQueue, clock: any MonotonicClock) {
        self.queue = queue
        self.clock = clock
    }

    func start(_ request: StartAudioMessage, delegate: any AudioSampleSourceDelegate) throws {
        dispatchPrecondition(condition: .onQueue(queue))
        stop()
        self.delegate = delegate
        generator = ToneGenerator()
        startUs = clock.nowMicroseconds()
        producedSamples = 0

        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now(), repeating: .milliseconds(Self.tickMilliseconds), leeway: .milliseconds(2))
        timer.setEventHandler { [weak self] in self?.produce() }
        timer.resume()
        self.timer = timer
    }

    func stop() {
        timer?.cancel()
        timer = nil
        delegate = nil
    }

    private func produce() {
        guard let delegate else { return }
        let elapsedUs = clock.nowMicroseconds() - startUs
        let due = elapsedUs * Int64(AudioChunk.sampleRate) / 1_000_000 - producedSamples
        guard due > 0 else { return }
        let timestamp = startUs + producedSamples * 1_000_000 / Int64(AudioChunk.sampleRate)
        let pcm = PCM16.encodeLittleEndian(generator.next(sampleCount: Int(due)))
        delegate.audioSource(didCapture: pcm, timestampUs: timestamp, discontinuity: producedSamples == 0)
        producedSamples += due
    }
}

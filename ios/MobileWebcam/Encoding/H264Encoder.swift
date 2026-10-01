import BridgeKit
import CoreMedia
import Foundation
import os
import VideoToolbox

struct EncoderSettings: Equatable, Sendable {
    var width: Int
    var height: Int
    var fps: Int
    var bitrateKbps: Int
    var mode: EncoderMode
}

/// One VTCompressionSession for one `VideoConfig`. A size, mode or rate change creates a new
/// encoder with a new `configId`, so every frame can be matched to the config it belongs to.
///
/// Frames are submitted on the video queue. VideoToolbox calls back on its own thread, where the
/// output is converted to an Annex-B `EncodedVideoFrame` right away and handed to `output`.
final class H264Encoder: @unchecked Sendable {
    /// Frames submitted to VideoToolbox but not yet returned. Beyond this the encoder is behind
    /// and new frames are skipped rather than queued (latency over completeness).
    private static let maxPendingFrames = 2

    let configId: Int
    let settings: EncoderSettings
    /// `standard` when low-latency rate control was requested but is not available.
    private(set) var effectiveMode: EncoderMode

    private var session: VTCompressionSession?
    private let pendingFrames = OSAllocatedUnfairLock(initialState: 0)
    private let output: @Sendable (EncodedVideoFrame) -> Void
    private let failure: @Sendable (OSStatus) -> Void
    private let logger: AppLogger

    init(configId: Int, settings: EncoderSettings, logger: AppLogger,
         output: @escaping @Sendable (EncodedVideoFrame) -> Void,
         failure: @escaping @Sendable (OSStatus) -> Void) throws {
        self.configId = configId
        self.settings = settings
        self.logger = logger
        self.output = output
        self.failure = failure
        effectiveMode = settings.mode

        let refcon = Unmanaged.passUnretained(self).toOpaque()
        if settings.mode == .lowLatency {
            do {
                session = try Self.makeSession(settings, lowLatency: true, refcon: refcon)
            } catch {
                logger.warn("encoder", "Low-latency rate control unavailable (\(error)); using standard encoder")
            }
        }
        if session == nil {
            session = try Self.makeSession(settings, lowLatency: false, refcon: refcon)
            effectiveMode = .standard
        }
        guard let session else { throw CaptureError.encoderFailed(kVTInvalidSessionErr) }
        Self.configure(session, settings: settings, lowLatency: effectiveMode == .lowLatency, logger: logger)
        let prepared = VTCompressionSessionPrepareToEncodeFrames(session)
        if prepared != noErr {
            invalidate()
            throw CaptureError.encoderFailed(prepared)
        }
    }

    deinit {
        invalidate()
    }

    /// Returns false when the frame was skipped (encoder behind) or rejected.
    @discardableResult
    func encode(_ pixelBuffer: CVPixelBuffer, presentationTimeUs: Int64, forceKeyframe: Bool) -> Bool {
        guard let session else { return false }
        let admitted = pendingFrames.withLock { pending -> Bool in
            guard pending < H264Encoder.maxPendingFrames else { return false }
            pending += 1
            return true
        }
        guard admitted else { return false }

        let frameProperties: CFDictionary? = forceKeyframe
            ? [kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary
            : nil
        let status = VTCompressionSessionEncodeFrame(
            session,
            imageBuffer: pixelBuffer,
            presentationTimeStamp: CMTime(value: presentationTimeUs, timescale: 1_000_000),
            duration: .invalid,
            frameProperties: frameProperties,
            sourceFrameRefcon: nil,
            infoFlagsOut: nil
        )
        guard status == noErr else {
            pendingFrames.withLock { $0 = max(0, $0 - 1) }
            failure(status)
            return false
        }
        return true
    }

    /// Stops the session. No callbacks arrive after this returns.
    func invalidate() {
        guard let session else { return }
        self.session = nil
        VTCompressionSessionInvalidate(session)
    }

    // MARK: - Output (VideoToolbox thread)

    fileprivate func handleOutput(status: OSStatus, infoFlags: VTEncodeInfoFlags, sampleBuffer: CMSampleBuffer?) {
        pendingFrames.withLock { $0 = max(0, $0 - 1) }
        guard status == noErr else {
            failure(status)
            return
        }
        guard !infoFlags.contains(.frameDropped), let sampleBuffer, CMSampleBufferDataIsReady(sampleBuffer) else { return }
        if let frame = Self.makeFrame(from: sampleBuffer, configId: configId) {
            output(frame)
        } else {
            logger.warn("encoder", "Dropped an access unit that could not be converted to Annex-B")
        }
    }

    // MARK: - Session setup

    private static func makeSession(_ settings: EncoderSettings, lowLatency: Bool,
                                    refcon: UnsafeMutableRawPointer) throws -> VTCompressionSession {
        var specification: [String: Any] = [:]
        if lowLatency {
            specification[kVTVideoEncoderSpecification_EnableLowLatencyRateControl as String] = true
        }
        var created: VTCompressionSession?
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            width: Int32(settings.width),
            height: Int32(settings.height),
            codecType: kCMVideoCodecType_H264,
            encoderSpecification: specification as CFDictionary,
            imageBufferAttributes: nil,
            compressedDataAllocator: nil,
            outputCallback: { refcon, _, status, infoFlags, sampleBuffer in
                guard let refcon else { return }
                let encoder = Unmanaged<H264Encoder>.fromOpaque(refcon).takeUnretainedValue()
                encoder.handleOutput(status: status, infoFlags: infoFlags, sampleBuffer: sampleBuffer)
            },
            refcon: refcon,
            compressionSessionOut: &created
        )
        guard status == noErr, let created else { throw CaptureError.encoderFailed(status) }
        return created
    }

    private static func configure(_ session: VTCompressionSession, settings: EncoderSettings,
                                  lowLatency: Bool, logger: AppLogger) {
        func set(_ key: CFString, _ value: CFTypeRef, _ name: String) {
            let status = VTSessionSetProperty(session, key: key, value: value)
            if status != noErr { logger.debug("encoder", "Property \(name) not applied (\(status))") }
        }
        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue, "RealTime")
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse, "AllowFrameReordering")
        set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_ConstrainedHigh_AutoLevel, "ProfileLevel")
        set(kVTCompressionPropertyKey_H264EntropyMode, kVTH264EntropyMode_CABAC, "H264EntropyMode")
        set(kVTCompressionPropertyKey_ExpectedFrameRate, NSNumber(value: settings.fps), "ExpectedFrameRate")
        set(kVTCompressionPropertyKey_AverageBitRate, NSNumber(value: settings.bitrateKbps * 1_000), "AverageBitRate")
        // Allow short bursts up to 1.5x the average, measured over one second.
        let bytesPerSecond = settings.bitrateKbps * 1_000 / 8 * 3 / 2
        set(kVTCompressionPropertyKey_DataRateLimits, [NSNumber(value: bytesPerSecond), NSNumber(value: 1)] as CFArray,
            "DataRateLimits")
        if !lowLatency {
            // Low-latency mode uses an infinite GOP and relies on RequestKeyframe instead.
            set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, NSNumber(value: 2), "MaxKeyFrameIntervalDuration")
        }
        set(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2, "ColorPrimaries")
        set(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2, "TransferFunction")
        set(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2, "YCbCrMatrix")
    }

    // MARK: - AVCC → Annex-B

    private static func makeFrame(from sampleBuffer: CMSampleBuffer, configId: Int) -> EncodedVideoFrame? {
        let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false) as? [[String: Any]]
        let first = attachments?.first
        let isKeyframe = !((first?[kCMSampleAttachmentKey_NotSync as String] as? Bool) ?? false)
        let isReferenced = (first?[kCMSampleAttachmentKey_IsDependedOnByOthers as String] as? Bool) ?? true

        guard let dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else { return nil }
        let length = CMBlockBufferGetDataLength(dataBuffer)
        var avcc = Data(count: length)
        let copyStatus = avcc.withUnsafeMutableBytes { raw -> OSStatus in
            guard let base = raw.baseAddress else { return kCMBlockBufferBadPointerParameterErr }
            return CMBlockBufferCopyDataBytes(dataBuffer, atOffset: 0, dataLength: length, destination: base)
        }
        guard copyStatus == kCMBlockBufferNoErr else { return nil }

        var nalLengthSize = 4
        var parameterSets: [Data] = []
        if let format = CMSampleBufferGetFormatDescription(sampleBuffer),
           let info = h264ParameterSets(of: format) {
            nalLengthSize = info.nalLengthSize
            if isKeyframe { parameterSets = info.sets }
        }
        guard let annexB = try? AnnexB.fromAVCC(avcc, lengthSize: nalLengthSize, prependingParameterSets: parameterSets) else {
            return nil
        }
        return EncodedVideoFrame(
            configId: configId,
            annexB: annexB,
            isKeyframe: isKeyframe,
            hasParameterSets: !parameterSets.isEmpty,
            isDisposable: !isKeyframe && !isReferenced,
            presentationTimeUs: HostTimeClock.microseconds(CMSampleBufferGetPresentationTimeStamp(sampleBuffer))
        )
    }

    private static func h264ParameterSets(of format: CMFormatDescription) -> (sets: [Data], nalLengthSize: Int)? {
        var count = 0
        var nalLengthSize: Int32 = 0
        guard CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            format, parameterSetIndex: 0, parameterSetPointerOut: nil, parameterSetSizeOut: nil,
            parameterSetCountOut: &count, nalUnitHeaderLengthOut: &nalLengthSize) == noErr
        else { return nil }

        var sets: [Data] = []
        for index in 0..<count {
            var pointer: UnsafePointer<UInt8>?
            var size = 0
            guard CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                format, parameterSetIndex: index, parameterSetPointerOut: &pointer, parameterSetSizeOut: &size,
                parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil) == noErr,
                let pointer
            else { return nil }
            sets.append(Data(bytes: pointer, count: size))
        }
        return (sets, Int(nalLengthSize))
    }
}

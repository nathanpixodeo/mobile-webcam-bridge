import AVFoundation

/// Reads linear PCM from an `AudioBufferList` as mono `Float` samples in [-1, 1], whatever the
/// sample format and channel layout the microphone delivers.
enum PCMSampleExtractor {
    struct UnsupportedFormat: Error, CustomStringConvertible {
        let description: String
    }

    static func monoSamples(from buffers: UnsafeMutableAudioBufferListPointer,
                            format: AudioStreamBasicDescription) throws -> [Float] {
        guard format.mFormatID == kAudioFormatLinearPCM else {
            throw UnsupportedFormat(description: "format id \(format.mFormatID) is not linear PCM")
        }
        let isFloat = format.mFormatFlags & kAudioFormatFlagIsFloat != 0
        let isNonInterleaved = format.mFormatFlags & kAudioFormatFlagIsNonInterleaved != 0
        let bits = Int(format.mBitsPerChannel)
        let channels = max(1, Int(format.mChannelsPerFrame))

        if isNonInterleaved {
            var mix: [Float] = []
            for buffer in buffers.prefix(channels) {
                let samples = try decode(buffer, isFloat: isFloat, bits: bits)
                if mix.isEmpty {
                    mix = samples
                } else {
                    for index in 0..<min(mix.count, samples.count) { mix[index] += samples[index] }
                }
            }
            return channels > 1 ? mix.map { $0 / Float(channels) } : mix
        }

        guard let buffer = buffers.first else { return [] }
        let interleaved = try decode(buffer, isFloat: isFloat, bits: bits)
        guard channels > 1 else { return interleaved }
        let frames = interleaved.count / channels
        var mono = [Float](repeating: 0, count: frames)
        for frame in 0..<frames {
            var sum: Float = 0
            for channel in 0..<channels { sum += interleaved[frame * channels + channel] }
            mono[frame] = sum / Float(channels)
        }
        return mono
    }

    private static func decode(_ buffer: AudioBuffer, isFloat: Bool, bits: Int) throws -> [Float] {
        guard let data = buffer.mData else { return [] }
        let byteCount = Int(buffer.mDataByteSize)
        switch (isFloat, bits) {
        case (true, 32):
            let samples = UnsafeBufferPointer(start: data.assumingMemoryBound(to: Float.self), count: byteCount / 4)
            return Array(samples)
        case (false, 16):
            let samples = UnsafeBufferPointer(start: data.assumingMemoryBound(to: Int16.self), count: byteCount / 2)
            return samples.map { Float($0) / 32_768 }
        case (false, 32):
            let samples = UnsafeBufferPointer(start: data.assumingMemoryBound(to: Int32.self), count: byteCount / 4)
            return samples.map { Float($0) / 2_147_483_648 }
        default:
            throw UnsupportedFormat(description: "\(bits)-bit \(isFloat ? "float" : "integer") samples")
        }
    }
}

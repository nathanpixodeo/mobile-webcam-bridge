import Foundation

/// Streaming linear-interpolation resampler for mono audio. Keeps its phase across calls so
/// chunk boundaries are seamless. Microphones normally already run at 48 kHz (the app asks for
/// it), in which case samples pass through untouched.
public struct LinearAudioResampler: Sendable {
    public let inputRate: Double
    public let outputRate: Double
    private let step: Double
    /// Position of the next output sample, in input samples, where index 0 is the last sample
    /// of the previous call and index 1 the first sample of the next call.
    private var position: Double = 1
    private var previous: Float = 0

    public init(inputRate: Double, outputRate: Double) {
        self.inputRate = inputRate
        self.outputRate = outputRate
        step = inputRate / outputRate
    }

    public var isPassthrough: Bool { inputRate == outputRate }

    public mutating func process(_ input: [Float]) -> [Float] {
        guard !isPassthrough else { return input }
        guard !input.isEmpty else { return [] }

        var output: [Float] = []
        output.reserveCapacity(Int(Double(input.count) / step) + 2)
        let count = input.count
        while true {
            let index = Int(position)
            guard index + 1 <= count else { break }
            let fraction = Float(position - Double(index))
            let a = index == 0 ? previous : input[index - 1]
            let b = input[index]
            output.append(a + (b - a) * fraction)
            position += step
        }
        position -= Double(count)
        previous = input[count - 1]
        return output
    }

    public mutating func reset() {
        position = 1
        previous = 0
    }
}

/// Conversion between float samples and the wire format (`s16le`).
public enum PCM16 {
    /// Clips to [-1, 1] and writes little-endian signed 16-bit samples.
    public static func encodeLittleEndian(_ samples: [Float]) -> Data {
        var data = Data(count: samples.count * 2)
        data.withUnsafeMutableBytes { raw in
            for (index, sample) in samples.enumerated() {
                let clipped = max(-1, min(1, sample))
                let value = Int16((clipped * 32_767).rounded())
                raw.storeBytes(of: value.littleEndian, toByteOffset: index * 2, as: Int16.self)
            }
        }
        return data
    }

    /// Reads little-endian signed 16-bit samples.
    public static func decodeLittleEndian(_ data: Data) -> [Int16] {
        let count = data.count / 2
        var samples = [Int16](repeating: 0, count: count)
        data.withUnsafeBytes { raw in
            for index in 0..<count {
                samples[index] = Int16(littleEndian: raw.loadUnaligned(fromByteOffset: index * 2, as: Int16.self))
            }
        }
        return samples
    }
}

/// Phase-continuous sine generator for the synthetic audio source.
public struct ToneGenerator: Sendable {
    public let frequency: Double
    public let sampleRate: Double
    public let amplitude: Float
    private var phase: Double = 0

    public init(frequency: Double = 440, sampleRate: Double = Double(AudioChunk.sampleRate), amplitude: Float = 0.25) {
        self.frequency = frequency
        self.sampleRate = sampleRate
        self.amplitude = amplitude
    }

    public mutating func next(sampleCount: Int) -> [Float] {
        let increment = 2 * Double.pi * frequency / sampleRate
        var samples = [Float](repeating: 0, count: max(0, sampleCount))
        for index in samples.indices {
            samples[index] = amplitude * Float(sin(phase))
            phase += increment
            if phase >= 2 * Double.pi { phase -= 2 * Double.pi }
        }
        return samples
    }
}

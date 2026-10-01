import Foundation
import XCTest
@testable import BridgeKit

final class AudioTests: XCTestCase {
    func testPCM16EncodingClipsAndRounds() {
        let data = PCM16.encodeLittleEndian([0, 1, -1, 2, -2, 0.5])
        XCTAssertEqual(PCM16.decodeLittleEndian(data), [0, 32_767, -32_767, 32_767, -32_767, 16_384])
        XCTAssertEqual(Data(PCM16.encodeLittleEndian([1]).prefix(2)).hex, "ff7f")
    }

    func testResamplerPassthrough() {
        var resampler = LinearAudioResampler(inputRate: 48_000, outputRate: 48_000)
        XCTAssertEqual(resampler.process([0.1, 0.2]), [0.1, 0.2])
    }

    func testResamplerProducesExpectedSampleCountAcrossChunks() {
        var resampler = LinearAudioResampler(inputRate: 44_100, outputRate: 48_000)
        var produced = 0
        for _ in 0..<100 {
            produced += resampler.process([Float](repeating: 0.5, count: 441)).count
        }
        // 1 s of 44.1 kHz input must give ~48 000 samples (±1 for phase).
        XCTAssertTrue(abs(produced - 48_000) <= 1, "produced \(produced)")
    }

    func testResamplerIsContinuousAtChunkBoundaries() {
        var resampler = LinearAudioResampler(inputRate: 24_000, outputRate: 48_000)
        let ramp = (0..<8).map { Float($0) }
        let output = resampler.process(Array(ramp[0..<4])) + resampler.process(Array(ramp[4..<8]))
        // Upsampling a linear ramp by 2 must give a finer linear ramp without jumps.
        for (lhs, rhs) in zip(output, output.dropFirst()) {
            XCTAssertEqual(rhs - lhs, 0.5, accuracy: 1e-5)
        }
    }

    func testToneGeneratorIsPhaseContinuous() {
        var split = ToneGenerator(frequency: 1_000, sampleRate: 48_000, amplitude: 1)
        var whole = ToneGenerator(frequency: 1_000, sampleRate: 48_000, amplitude: 1)
        let joined = split.next(sampleCount: 100) + split.next(sampleCount: 100)
        let reference = whole.next(sampleCount: 200)
        for (lhs, rhs) in zip(joined, reference) {
            XCTAssertEqual(lhs, rhs, accuracy: 1e-5)
        }
    }

    func testAudioChunkSplitKeepsTimingAndDiscontinuityOnFirstOnly() {
        let pcm = Data(count: (AudioChunk.maxSamplesPerChunk * 2 + 100) * 2)
        let chunks = AudioChunk.split(pcm: pcm, configId: 7, timestampUs: 1_000, isDiscontinuity: true)
        XCTAssertEqual(chunks.map(\.sampleCount), [1_920, 1_920, 100])
        XCTAssertEqual(chunks.map(\.isDiscontinuity), [true, false, false])
        XCTAssertEqual(chunks.map(\.timestampUs), [1_000, 41_000, 81_000])
        XCTAssertEqual(chunks[0].durationUs, 40_000)
    }
}

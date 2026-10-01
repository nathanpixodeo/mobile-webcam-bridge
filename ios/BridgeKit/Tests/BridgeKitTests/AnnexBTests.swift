import Foundation
import XCTest
@testable import BridgeKit

final class AnnexBTests: XCTestCase {
    func testSplitVectors() throws {
        for vector in try H264Vectors.load().split {
            let stream = Data(hex: vector.annexB)
            let units = AnnexB.splitNALUnits(stream)
            XCTAssertEqual(units.map(\.hex), vector.nals.map(\.hex), vector.name)
            XCTAssertEqual(units.compactMap(AnnexB.nalType(of:)), vector.nals.map(\.type), vector.name)
            XCTAssertEqual(AnnexB.containsIDR(stream), vector.isIdr, vector.name)
            XCTAssertEqual(AnnexB.containsParameterSets(stream), vector.hasParameterSets, vector.name)
        }
    }

    func testAVCCConversionVectors() throws {
        for vector in try H264Vectors.load().avccToAnnexB {
            let parameterSets = [vector.sps, vector.pps].compactMap { $0 }.map { Data(hex: $0) }
            let annexB = try AnnexB.fromAVCC(Data(hex: vector.avcc), prependingParameterSets: parameterSets)
            XCTAssertEqual(annexB.hex, vector.annexB, vector.name)
        }
    }

    func testAccessUnitDelimiterMatchesVector() throws {
        XCTAssertEqual(AnnexB.accessUnitDelimiter.hex, try H264Vectors.load().accessUnitDelimiter)
    }

    func testTruncatedAVCCIsRejected() {
        XCTAssertThrowsError(try AnnexB.fromAVCC(Data(hex: "0000000a6588")))
        XCTAssertThrowsError(try AnnexB.fromAVCC(Data(hex: "0000")))
    }

    func testConversionWorksOnDataSlices() throws {
        let padded = Data(hex: "ffff" + "00000002" + "0605")
        let slice = padded[2...]
        XCTAssertEqual(try AnnexB.fromAVCC(slice).hex, "000000010605")
    }
}

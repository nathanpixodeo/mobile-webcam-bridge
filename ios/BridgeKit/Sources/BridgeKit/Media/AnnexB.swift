import Foundation

/// H.264 byte-stream helpers. VideoToolbox emits AVCC (length-prefixed NAL units); the wire
/// protocol carries Annex-B (start-code-prefixed NAL units, SPEC §3.3).
public enum AnnexB {
    public static let startCode = Data([0x00, 0x00, 0x00, 0x01])
    /// Access unit delimiter NAL (`primary_pic_type` = 7), what the host appends after each AU.
    public static let accessUnitDelimiter = Data([0x00, 0x00, 0x00, 0x01, 0x09, 0xF0])

    public enum NALType {
        public static let nonIDRSlice: UInt8 = 1
        public static let idrSlice: UInt8 = 5
        public static let sei: UInt8 = 6
        public static let sps: UInt8 = 7
        public static let pps: UInt8 = 8
        public static let accessUnitDelimiter: UInt8 = 9
    }

    public enum ConversionError: Error, Equatable, Sendable {
        case unsupportedLengthSize(Int)
        case truncatedNAL(offset: Int)
    }

    /// `nal_unit_type` of a NAL unit given without its start code.
    public static func nalType(of nal: Data) -> UInt8? {
        nal.first.map { $0 & 0x1F }
    }

    /// Splits an Annex-B stream into NAL units (without start codes). Accepts 3- and 4-byte start
    /// codes and strips `trailing_zero_8bits`, which never belong to a NAL unit.
    public static func splitNALUnits(_ stream: Data) -> [Data] {
        let bytes = [UInt8](stream)
        var units: [Data] = []
        var unitStart: Int?
        var index = 0

        func closeUnit(at end: Int) {
            guard let start = unitStart else { return }
            var trimmedEnd = end
            while trimmedEnd > start && bytes[trimmedEnd - 1] == 0 { trimmedEnd -= 1 }
            if trimmedEnd > start { units.append(Data(bytes[start..<trimmedEnd])) }
        }

        while index + 2 < bytes.count {
            if bytes[index] == 0, bytes[index + 1] == 0, bytes[index + 2] == 1 {
                closeUnit(at: index)
                unitStart = index + 3
                index += 3
            } else {
                index += 1
            }
        }
        closeUnit(at: bytes.count)
        return units
    }

    public static func containsIDR(_ stream: Data) -> Bool {
        splitNALUnits(stream).contains { nalType(of: $0) == NALType.idrSlice }
    }

    public static func containsParameterSets(_ stream: Data) -> Bool {
        let types = Set(splitNALUnits(stream).compactMap(nalType(of:)))
        return types.contains(NALType.sps) && types.contains(NALType.pps)
    }

    /// Converts length-prefixed AVCC data into Annex-B, optionally prefixing parameter sets
    /// (SPS, PPS, given without start codes) so every IDR is self-contained.
    public static func fromAVCC(_ avcc: Data, lengthSize: Int = 4,
                                prependingParameterSets parameterSets: [Data] = []) throws -> Data {
        guard (1...4).contains(lengthSize) else { throw ConversionError.unsupportedLengthSize(lengthSize) }

        var output = Data()
        output.reserveCapacity(avcc.count + parameterSets.reduce(0) { $0 + $1.count + startCode.count } + 16)
        for parameterSet in parameterSets {
            output.append(startCode)
            output.append(parameterSet)
        }

        var offset = avcc.startIndex
        while offset < avcc.endIndex {
            guard avcc.endIndex - offset >= lengthSize else {
                throw ConversionError.truncatedNAL(offset: offset - avcc.startIndex)
            }
            var length = 0
            for byteIndex in offset..<(offset + lengthSize) {
                length = (length << 8) | Int(avcc[byteIndex])
            }
            offset += lengthSize
            guard avcc.endIndex - offset >= length else {
                throw ConversionError.truncatedNAL(offset: offset - avcc.startIndex)
            }
            output.append(startCode)
            output.append(avcc[offset..<(offset + length)])
            offset += length
        }
        return output
    }
}

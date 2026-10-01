import Foundation

/// Appends fixed-width integers in network (big-endian) byte order.
struct ByteWriter {
    private(set) var data: Data

    init(capacity: Int) {
        data = Data()
        data.reserveCapacity(capacity)
    }

    mutating func write(_ value: UInt8) {
        data.append(value)
    }

    mutating func write<T: FixedWidthInteger>(bigEndian value: T) {
        withUnsafeBytes(of: value.bigEndian) { data.append(contentsOf: $0) }
    }

    mutating func write(_ bytes: Data) {
        data.append(bytes)
    }
}

/// Reads fixed-width big-endian integers from `Data`, including slices whose `startIndex` is
/// not zero.
struct ByteReader {
    private let bytes: Data
    private var offset: Data.Index

    init(_ bytes: Data) {
        self.bytes = bytes
        offset = bytes.startIndex
    }

    var remainingCount: Int { bytes.endIndex - offset }

    mutating func readUInt8() -> UInt8? {
        guard remainingCount >= 1 else { return nil }
        defer { offset += 1 }
        return bytes[offset]
    }

    mutating func readUnsigned<T: FixedWidthInteger & UnsignedInteger>(_: T.Type) -> T? {
        let size = MemoryLayout<T>.size
        guard remainingCount >= size else { return nil }
        var value: T = 0
        for index in offset..<(offset + size) {
            value = (value << 8) | T(bytes[index])
        }
        offset += size
        return value
    }

    mutating func readInt64() -> Int64? {
        readUnsigned(UInt64.self).map { Int64(bitPattern: $0) }
    }
}

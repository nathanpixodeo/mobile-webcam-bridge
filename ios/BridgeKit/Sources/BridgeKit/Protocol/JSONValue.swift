/// JSON document model restricted to what canonical JSON allows: no floating-point numbers.
public enum JSONValue: Equatable, Sendable {
    case null
    case bool(Bool)
    case integer(Int64)
    case string(String)
    case array([JSONValue])
    case object([String: JSONValue])
}

/// Converts any `Encodable` into a `JSONValue` tree. Unlike `JSONEncoder` it keeps no key order
/// at all — ordering is decided by `CanonicalJSON` — and it rejects non-integral numbers.
struct JSONValueEncoder {
    func encode<T: Encodable>(_ value: T) throws -> JSONValue {
        let root = EncodingNode()
        try value.encode(to: TreeEncoder(node: root, codingPath: []))
        return root.resolve()
    }
}

// MARK: - Tree storage

/// Reference-typed node so nested containers can keep writing after they are handed out.
private final class EncodingNode {
    enum Content {
        case empty
        case value(JSONValue)
        case object(ObjectStorage)
        case array(ArrayStorage)
    }

    var content: Content = .empty

    func objectStorage() -> ObjectStorage {
        if case .object(let existing) = content { return existing }
        let storage = ObjectStorage()
        content = .object(storage)
        return storage
    }

    func arrayStorage() -> ArrayStorage {
        if case .array(let existing) = content { return existing }
        let storage = ArrayStorage()
        content = .array(storage)
        return storage
    }

    func resolve() -> JSONValue {
        switch content {
        case .empty: .object([:])
        case .value(let value): value
        case .object(let storage): .object(storage.entries.mapValues { $0.resolve() })
        case .array(let storage): .array(storage.items.map { $0.resolve() })
        }
    }
}

private final class ObjectStorage {
    var entries: [String: EncodingNode] = [:]

    func child(forKey key: String) -> EncodingNode {
        let node = EncodingNode()
        entries[key] = node
        return node
    }
}

private final class ArrayStorage {
    var items: [EncodingNode] = []

    func appendChild() -> EncodingNode {
        let node = EncodingNode()
        items.append(node)
        return node
    }
}

private struct IndexKey: CodingKey {
    let intValue: Int?
    let stringValue: String

    init(_ index: Int) {
        intValue = index
        stringValue = "Index \(index)"
    }

    init?(stringValue: String) { return nil }
    init?(intValue: Int) { self.init(intValue) }
}

private struct NamedKey: CodingKey {
    let stringValue: String
    var intValue: Int? { nil }

    init(_ name: String) { stringValue = name }
    init?(stringValue: String) { self.stringValue = stringValue }
    init?(intValue: Int) { return nil }
}

// MARK: - Scalar conversion

private enum Scalar {
    static func integer<T: BinaryInteger>(_ value: T, _ path: [any CodingKey]) throws -> JSONValue {
        guard let exact = Int64(exactly: value) else {
            throw EncodingError.invalidValue(value, .init(codingPath: path, debugDescription: "Integer outside Int64 range"))
        }
        return .integer(exact)
    }

    static func floatingPoint<T: BinaryFloatingPoint>(_ value: T, _ path: [any CodingKey]) throws -> JSONValue {
        guard value.isFinite, let exact = Int64(exactly: value) else {
            throw EncodingError.invalidValue(value, .init(codingPath: path, debugDescription: "Canonical JSON only allows integers"))
        }
        return .integer(exact)
    }
}

// MARK: - Encoder

private struct TreeEncoder: Encoder {
    let node: EncodingNode
    let codingPath: [any CodingKey]
    var userInfo: [CodingUserInfoKey: Any] { [:] }

    func container<Key: CodingKey>(keyedBy type: Key.Type) -> KeyedEncodingContainer<Key> {
        KeyedEncodingContainer(KeyedContainer<Key>(storage: node.objectStorage(), codingPath: codingPath))
    }

    func unkeyedContainer() -> any UnkeyedEncodingContainer {
        UnkeyedContainer(storage: node.arrayStorage(), codingPath: codingPath)
    }

    func singleValueContainer() -> any SingleValueEncodingContainer {
        SingleValueContainer(node: node, codingPath: codingPath)
    }
}

private struct KeyedContainer<Key: CodingKey>: KeyedEncodingContainerProtocol {
    let storage: ObjectStorage
    let codingPath: [any CodingKey]

    private func set(_ value: JSONValue, _ key: Key) {
        storage.child(forKey: key.stringValue).content = .value(value)
    }

    private func path(_ key: Key) -> [any CodingKey] { codingPath + [key] }

    mutating func encodeNil(forKey key: Key) throws { set(.null, key) }
    mutating func encode(_ value: Bool, forKey key: Key) throws { set(.bool(value), key) }
    mutating func encode(_ value: String, forKey key: Key) throws { set(.string(value), key) }
    mutating func encode(_ value: Double, forKey key: Key) throws { set(try Scalar.floatingPoint(value, path(key)), key) }
    mutating func encode(_ value: Float, forKey key: Key) throws { set(try Scalar.floatingPoint(value, path(key)), key) }
    mutating func encode(_ value: Int, forKey key: Key) throws { set(try Scalar.integer(value, path(key)), key) }
    mutating func encode(_ value: Int8, forKey key: Key) throws { set(try Scalar.integer(value, path(key)), key) }
    mutating func encode(_ value: Int16, forKey key: Key) throws { set(try Scalar.integer(value, path(key)), key) }
    mutating func encode(_ value: Int32, forKey key: Key) throws { set(try Scalar.integer(value, path(key)), key) }
    mutating func encode(_ value: Int64, forKey key: Key) throws { set(try Scalar.integer(value, path(key)), key) }
    mutating func encode(_ value: UInt, forKey key: Key) throws { set(try Scalar.integer(value, path(key)), key) }
    mutating func encode(_ value: UInt8, forKey key: Key) throws { set(try Scalar.integer(value, path(key)), key) }
    mutating func encode(_ value: UInt16, forKey key: Key) throws { set(try Scalar.integer(value, path(key)), key) }
    mutating func encode(_ value: UInt32, forKey key: Key) throws { set(try Scalar.integer(value, path(key)), key) }
    mutating func encode(_ value: UInt64, forKey key: Key) throws { set(try Scalar.integer(value, path(key)), key) }

    mutating func encode<T: Encodable>(_ value: T, forKey key: Key) throws {
        let child = storage.child(forKey: key.stringValue)
        try value.encode(to: TreeEncoder(node: child, codingPath: path(key)))
    }

    mutating func nestedContainer<NestedKey: CodingKey>(keyedBy keyType: NestedKey.Type,
                                                         forKey key: Key) -> KeyedEncodingContainer<NestedKey> {
        let child = storage.child(forKey: key.stringValue)
        return KeyedEncodingContainer(KeyedContainer<NestedKey>(storage: child.objectStorage(), codingPath: path(key)))
    }

    mutating func nestedUnkeyedContainer(forKey key: Key) -> any UnkeyedEncodingContainer {
        let child = storage.child(forKey: key.stringValue)
        return UnkeyedContainer(storage: child.arrayStorage(), codingPath: path(key))
    }

    mutating func superEncoder() -> any Encoder {
        TreeEncoder(node: storage.child(forKey: "super"), codingPath: codingPath + [NamedKey("super")])
    }

    mutating func superEncoder(forKey key: Key) -> any Encoder {
        TreeEncoder(node: storage.child(forKey: key.stringValue), codingPath: path(key))
    }
}

private struct UnkeyedContainer: UnkeyedEncodingContainer {
    let storage: ArrayStorage
    let codingPath: [any CodingKey]
    var count: Int { storage.items.count }

    private func append(_ value: JSONValue) {
        storage.appendChild().content = .value(value)
    }

    private var nextPath: [any CodingKey] { codingPath + [IndexKey(count)] }

    mutating func encodeNil() throws { append(.null) }
    mutating func encode(_ value: Bool) throws { append(.bool(value)) }
    mutating func encode(_ value: String) throws { append(.string(value)) }
    mutating func encode(_ value: Double) throws { append(try Scalar.floatingPoint(value, nextPath)) }
    mutating func encode(_ value: Float) throws { append(try Scalar.floatingPoint(value, nextPath)) }
    mutating func encode(_ value: Int) throws { append(try Scalar.integer(value, nextPath)) }
    mutating func encode(_ value: Int8) throws { append(try Scalar.integer(value, nextPath)) }
    mutating func encode(_ value: Int16) throws { append(try Scalar.integer(value, nextPath)) }
    mutating func encode(_ value: Int32) throws { append(try Scalar.integer(value, nextPath)) }
    mutating func encode(_ value: Int64) throws { append(try Scalar.integer(value, nextPath)) }
    mutating func encode(_ value: UInt) throws { append(try Scalar.integer(value, nextPath)) }
    mutating func encode(_ value: UInt8) throws { append(try Scalar.integer(value, nextPath)) }
    mutating func encode(_ value: UInt16) throws { append(try Scalar.integer(value, nextPath)) }
    mutating func encode(_ value: UInt32) throws { append(try Scalar.integer(value, nextPath)) }
    mutating func encode(_ value: UInt64) throws { append(try Scalar.integer(value, nextPath)) }

    mutating func encode<T: Encodable>(_ value: T) throws {
        let path = nextPath
        let child = storage.appendChild()
        try value.encode(to: TreeEncoder(node: child, codingPath: path))
    }

    mutating func nestedContainer<NestedKey: CodingKey>(keyedBy keyType: NestedKey.Type) -> KeyedEncodingContainer<NestedKey> {
        let path = nextPath
        let child = storage.appendChild()
        return KeyedEncodingContainer(KeyedContainer<NestedKey>(storage: child.objectStorage(), codingPath: path))
    }

    mutating func nestedUnkeyedContainer() -> any UnkeyedEncodingContainer {
        let path = nextPath
        let child = storage.appendChild()
        return UnkeyedContainer(storage: child.arrayStorage(), codingPath: path)
    }

    mutating func superEncoder() -> any Encoder {
        let path = nextPath
        return TreeEncoder(node: storage.appendChild(), codingPath: path)
    }
}

private struct SingleValueContainer: SingleValueEncodingContainer {
    let node: EncodingNode
    let codingPath: [any CodingKey]

    private func set(_ value: JSONValue) { node.content = .value(value) }

    mutating func encodeNil() throws { set(.null) }
    mutating func encode(_ value: Bool) throws { set(.bool(value)) }
    mutating func encode(_ value: String) throws { set(.string(value)) }
    mutating func encode(_ value: Double) throws { set(try Scalar.floatingPoint(value, codingPath)) }
    mutating func encode(_ value: Float) throws { set(try Scalar.floatingPoint(value, codingPath)) }
    mutating func encode(_ value: Int) throws { set(try Scalar.integer(value, codingPath)) }
    mutating func encode(_ value: Int8) throws { set(try Scalar.integer(value, codingPath)) }
    mutating func encode(_ value: Int16) throws { set(try Scalar.integer(value, codingPath)) }
    mutating func encode(_ value: Int32) throws { set(try Scalar.integer(value, codingPath)) }
    mutating func encode(_ value: Int64) throws { set(try Scalar.integer(value, codingPath)) }
    mutating func encode(_ value: UInt) throws { set(try Scalar.integer(value, codingPath)) }
    mutating func encode(_ value: UInt8) throws { set(try Scalar.integer(value, codingPath)) }
    mutating func encode(_ value: UInt16) throws { set(try Scalar.integer(value, codingPath)) }
    mutating func encode(_ value: UInt32) throws { set(try Scalar.integer(value, codingPath)) }
    mutating func encode(_ value: UInt64) throws { set(try Scalar.integer(value, codingPath)) }

    mutating func encode<T: Encodable>(_ value: T) throws {
        try value.encode(to: TreeEncoder(node: node, codingPath: codingPath))
    }
}

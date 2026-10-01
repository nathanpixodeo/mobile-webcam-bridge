import Foundation

/// Canonical JSON as required by SPEC §4: UTF-8, object keys sorted by Unicode scalar value,
/// no insignificant whitespace, no escaped forward slashes, integers only.
///
/// Key order is implemented here explicitly instead of relying on `JSONEncoder.sortedKeys`,
/// whose ordering rules differ between Foundation implementations.
public enum CanonicalJSON {
    public static func encode<T: Encodable>(_ value: T) throws -> Data {
        Data(try string(value).utf8)
    }

    public static func string<T: Encodable>(_ value: T) throws -> String {
        serialize(try JSONValueEncoder().encode(value))
    }

    public static func serialize(_ value: JSONValue) -> String {
        var output = ""
        append(value, to: &output)
        return output
    }

    private static func append(_ value: JSONValue, to output: inout String) {
        switch value {
        case .null:
            output += "null"
        case .bool(let flag):
            output += flag ? "true" : "false"
        case .integer(let number):
            output += String(number)
        case .string(let text):
            appendQuoted(text, to: &output)
        case .array(let items):
            output += "["
            for (index, item) in items.enumerated() {
                if index > 0 { output += "," }
                append(item, to: &output)
            }
            output += "]"
        case .object(let entries):
            output += "{"
            let keys = entries.keys.sorted { $0.unicodeScalars.lexicographicallyPrecedes($1.unicodeScalars) }
            for (index, key) in keys.enumerated() {
                if index > 0 { output += "," }
                appendQuoted(key, to: &output)
                output += ":"
                if let entry = entries[key] { append(entry, to: &output) }
            }
            output += "}"
        }
    }

    private static func appendQuoted(_ text: String, to output: inout String) {
        output += "\""
        for scalar in text.unicodeScalars {
            switch scalar {
            case "\"": output += "\\\""
            case "\\": output += "\\\\"
            case "\u{08}": output += "\\b"
            case "\u{0C}": output += "\\f"
            case "\n": output += "\\n"
            case "\r": output += "\\r"
            case "\t": output += "\\t"
            case _ where scalar.value < 0x20:
                let hex = String(scalar.value, radix: 16)
                output += "\\u" + String(repeating: "0", count: 4 - hex.count) + hex
            default:
                output.unicodeScalars.append(scalar)
            }
        }
        output += "\""
    }
}

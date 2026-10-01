// Minimal JSON support: a streaming writer for compact output and a strict parser for the
// JSON-lines commands the video hub reads from stdin. Pure C++, no Windows dependencies.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mwb::native {

// Builds compact JSON (no whitespace). Misuse (e.g. a value without a key inside an object)
// throws std::logic_error, because it is always a programming error.
class JsonWriter {
public:
    JsonWriter& BeginObject();
    JsonWriter& EndObject();
    JsonWriter& BeginArray();
    JsonWriter& EndArray();
    JsonWriter& Key(std::string_view key);

    JsonWriter& String(std::string_view utf8);
    JsonWriter& String(std::wstring_view text);
    JsonWriter& Int(std::int64_t value);
    JsonWriter& UInt(std::uint64_t value);
    JsonWriter& Bool(bool value);
    JsonWriter& Null();

    // Inserts an already-serialised JSON value verbatim.
    JsonWriter& Raw(std::string_view json);

    template <typename T>
    JsonWriter& Field(std::string_view key, const T& value) {
        Key(key);
        return Value(value);
    }

    // Moves the finished document out; throws if objects/arrays are still open.
    [[nodiscard]] std::string Take();

private:
    enum class Scope { Object, Array };
    struct Frame {
        Scope scope;
        bool empty = true;
        bool expectingValue = false;  // object only: a key was written, its value is due
    };

    JsonWriter& Value(std::string_view v) { return String(v); }
    JsonWriter& Value(const std::string& v) { return String(std::string_view(v)); }
    JsonWriter& Value(const char* v) { return String(std::string_view(v)); }
    JsonWriter& Value(std::wstring_view v) { return String(v); }
    JsonWriter& Value(const std::wstring& v) { return String(std::wstring_view(v)); }
    JsonWriter& Value(bool v) { return Bool(v); }
    JsonWriter& Value(std::int32_t v) { return Int(v); }
    JsonWriter& Value(std::int64_t v) { return Int(v); }
    JsonWriter& Value(std::uint32_t v) { return UInt(v); }
    JsonWriter& Value(std::uint64_t v) { return UInt(v); }
    JsonWriter& Value(unsigned long v) { return UInt(v); }
    JsonWriter& Value(long v) { return Int(v); }
    JsonWriter& Value(std::nullptr_t) { return Null(); }

    void BeforeValue();
    void AppendEscaped(std::string_view utf8);

    std::string out_;
    std::vector<Frame> stack_;
    bool rootWritten_ = false;
};

class JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonMember = std::pair<std::string, JsonValue>;
using JsonObject = std::vector<JsonMember>;

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;
    explicit JsonValue(bool value) : value_(value) {}
    explicit JsonValue(double value) : value_(value) {}
    explicit JsonValue(std::string value) : value_(std::move(value)) {}
    explicit JsonValue(JsonArray value) : value_(std::move(value)) {}
    explicit JsonValue(JsonObject value) : value_(std::move(value)) {}

    [[nodiscard]] Type GetType() const noexcept { return static_cast<Type>(value_.index()); }
    [[nodiscard]] bool IsNull() const noexcept { return GetType() == Type::Null; }

    [[nodiscard]] const bool* AsBool() const noexcept { return std::get_if<bool>(&value_); }
    [[nodiscard]] const double* AsNumber() const noexcept { return std::get_if<double>(&value_); }
    [[nodiscard]] const std::string* AsString() const noexcept { return std::get_if<std::string>(&value_); }
    [[nodiscard]] const JsonArray* AsArray() const noexcept { return std::get_if<JsonArray>(&value_); }
    [[nodiscard]] const JsonObject* AsObject() const noexcept { return std::get_if<JsonObject>(&value_); }

    // Object member lookup; nullptr when this is not an object or the key is absent.
    [[nodiscard]] const JsonValue* Find(std::string_view key) const noexcept;

private:
    // Order matches Type.
    std::variant<std::monostate, bool, double, std::string, JsonArray, JsonObject> value_;
};

class JsonParseError : public std::runtime_error {
public:
    JsonParseError(const std::string& message, std::size_t offset)
        : std::runtime_error(message + " at offset " + std::to_string(offset)), offset_(offset) {}
    [[nodiscard]] std::size_t Offset() const noexcept { return offset_; }

private:
    std::size_t offset_;
};

struct JsonParseLimits {
    std::size_t maxDepth = 16;
    std::size_t maxLength = 64 * 1024;
};

// Parses exactly one JSON value (RFC 8259, strict: no comments, no trailing commas, no
// duplicate keys, valid UTF-8 escapes). Surrounding whitespace is allowed.
[[nodiscard]] JsonValue ParseJson(std::string_view text, const JsonParseLimits& limits = {});

}  // namespace mwb::native

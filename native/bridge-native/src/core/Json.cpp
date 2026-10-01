#include "core/Json.h"

#include <charconv>
#include <cmath>
#include <system_error>

namespace mwb::native {

namespace {

// Decodes one UTF-8 sequence starting at `text[pos]`. Returns its length in bytes, or 0 when the
// sequence is invalid (bad lead/continuation byte, overlong form, surrogate, > U+10FFFF).
std::size_t DecodeUtf8(std::string_view text, std::size_t pos, char32_t& codePoint) noexcept {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(pos);
    if (lead < 0x80) {
        codePoint = lead;
        return 1;
    }

    std::size_t length = 0;
    char32_t value = 0;
    char32_t minimum = 0;
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        value = lead & 0x1F;
        minimum = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        value = lead & 0x0F;
        minimum = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        value = lead & 0x07;
        minimum = 0x10000;
    } else {
        return 0;
    }
    if (pos + length > text.size()) return 0;
    for (std::size_t i = 1; i < length; ++i) {
        const unsigned char next = byte(pos + i);
        if ((next & 0xC0) != 0x80) return 0;
        value = (value << 6) | (next & 0x3F);
    }
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return 0;
    codePoint = value;
    return length;
}

void AppendUtf8(std::string& out, char32_t codePoint) {
    if (codePoint < 0x80) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// JsonWriter
// ---------------------------------------------------------------------------------------------

void JsonWriter::BeforeValue() {
    if (stack_.empty()) {
        if (rootWritten_) throw std::logic_error("JsonWriter: more than one root value");
        rootWritten_ = true;
        return;
    }
    Frame& top = stack_.back();
    if (top.scope == Scope::Array) {
        if (!top.empty) out_.push_back(',');
        top.empty = false;
        return;
    }
    if (!top.expectingValue) throw std::logic_error("JsonWriter: object value written without a key");
    top.expectingValue = false;
}

JsonWriter& JsonWriter::BeginObject() {
    BeforeValue();
    out_.push_back('{');
    stack_.push_back(Frame{Scope::Object});
    return *this;
}

JsonWriter& JsonWriter::EndObject() {
    if (stack_.empty() || stack_.back().scope != Scope::Object || stack_.back().expectingValue) {
        throw std::logic_error("JsonWriter: unbalanced EndObject");
    }
    stack_.pop_back();
    out_.push_back('}');
    return *this;
}

JsonWriter& JsonWriter::BeginArray() {
    BeforeValue();
    out_.push_back('[');
    stack_.push_back(Frame{Scope::Array});
    return *this;
}

JsonWriter& JsonWriter::EndArray() {
    if (stack_.empty() || stack_.back().scope != Scope::Array) throw std::logic_error("JsonWriter: unbalanced EndArray");
    stack_.pop_back();
    out_.push_back(']');
    return *this;
}

JsonWriter& JsonWriter::Key(std::string_view key) {
    if (stack_.empty() || stack_.back().scope != Scope::Object || stack_.back().expectingValue) {
        throw std::logic_error("JsonWriter: key outside of an object");
    }
    Frame& top = stack_.back();
    if (!top.empty) out_.push_back(',');
    top.empty = false;
    top.expectingValue = true;
    AppendEscaped(key);
    out_.push_back(':');
    return *this;
}

JsonWriter& JsonWriter::String(std::string_view utf8) {
    BeforeValue();
    AppendEscaped(utf8);
    return *this;
}

JsonWriter& JsonWriter::String(std::wstring_view text) {
    // Manual UTF-16 → UTF-8 so the writer stays free of Windows APIs. Unpaired surrogates
    // become U+FFFD.
    std::string utf8;
    utf8.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t cp = static_cast<char16_t>(text[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 1 < text.size()) {
                const char32_t low = static_cast<char16_t>(text[i + 1]);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    ++i;
                } else {
                    cp = 0xFFFD;
                }
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        AppendUtf8(utf8, cp);
    }
    return String(std::string_view(utf8));
}

JsonWriter& JsonWriter::Int(std::int64_t value) {
    BeforeValue();
    out_ += std::to_string(value);
    return *this;
}

JsonWriter& JsonWriter::UInt(std::uint64_t value) {
    BeforeValue();
    out_ += std::to_string(value);
    return *this;
}

JsonWriter& JsonWriter::Bool(bool value) {
    BeforeValue();
    out_ += value ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::Null() {
    BeforeValue();
    out_ += "null";
    return *this;
}

JsonWriter& JsonWriter::Raw(std::string_view json) {
    BeforeValue();
    out_ += json;
    return *this;
}

std::string JsonWriter::Take() {
    if (!stack_.empty() || !rootWritten_) throw std::logic_error("JsonWriter: document is incomplete");
    rootWritten_ = false;
    return std::move(out_);
}

void JsonWriter::AppendEscaped(std::string_view utf8) {
    static constexpr char kHex[] = "0123456789abcdef";
    out_.push_back('"');
    std::size_t pos = 0;
    while (pos < utf8.size()) {
        const auto c = static_cast<unsigned char>(utf8[pos]);
        if (c >= 0x80) {
            char32_t cp = 0;
            const std::size_t length = DecodeUtf8(utf8, pos, cp);
            if (length == 0) {
                out_ += "\\ufffd";
                ++pos;
            } else {
                out_.append(utf8.substr(pos, length));
                pos += length;
            }
            continue;
        }
        switch (c) {
            case '"': out_ += "\\\""; break;
            case '\\': out_ += "\\\\"; break;
            case '\b': out_ += "\\b"; break;
            case '\f': out_ += "\\f"; break;
            case '\n': out_ += "\\n"; break;
            case '\r': out_ += "\\r"; break;
            case '\t': out_ += "\\t"; break;
            default:
                if (c < 0x20) {
                    out_ += "\\u00";
                    out_.push_back(kHex[c >> 4]);
                    out_.push_back(kHex[c & 0x0F]);
                } else {
                    out_.push_back(static_cast<char>(c));
                }
        }
        ++pos;
    }
    out_.push_back('"');
}

// ---------------------------------------------------------------------------------------------
// JsonValue
// ---------------------------------------------------------------------------------------------

const JsonValue* JsonValue::Find(std::string_view key) const noexcept {
    const JsonObject* object = AsObject();
    if (object == nullptr) return nullptr;
    for (const JsonMember& member : *object) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------------------------

namespace {

class Parser {
public:
    Parser(std::string_view text, const JsonParseLimits& limits) : text_(text), limits_(limits) {}

    JsonValue ParseDocument() {
        if (text_.size() > limits_.maxLength) Fail("document too large");
        SkipWhitespace();
        JsonValue value = ParseValue(0);
        SkipWhitespace();
        if (pos_ != text_.size()) Fail("unexpected trailing characters");
        return value;
    }

private:
    [[noreturn]] void Fail(const char* message) const { throw JsonParseError(message, pos_); }

    [[nodiscard]] bool AtEnd() const noexcept { return pos_ >= text_.size(); }
    [[nodiscard]] char Peek() const noexcept { return AtEnd() ? '\0' : text_[pos_]; }

    void SkipWhitespace() noexcept {
        while (!AtEnd()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++pos_;
        }
    }

    void Expect(char c) {
        if (Peek() != c) Fail("unexpected character");
        ++pos_;
    }

    void ExpectLiteral(std::string_view literal) {
        if (text_.substr(pos_, literal.size()) != literal) Fail("invalid literal");
        pos_ += literal.size();
    }

    JsonValue ParseValue(std::size_t depth) {
        if (depth > limits_.maxDepth) Fail("nesting too deep");
        switch (Peek()) {
            case '{': return ParseObject(depth);
            case '[': return ParseArray(depth);
            case '"': return JsonValue(ParseString());
            case 't': ExpectLiteral("true"); return JsonValue(true);
            case 'f': ExpectLiteral("false"); return JsonValue(false);
            case 'n': ExpectLiteral("null"); return JsonValue();
            default:
                if (Peek() == '-' || (Peek() >= '0' && Peek() <= '9')) return JsonValue(ParseNumber());
                Fail("unexpected character");
        }
    }

    JsonValue ParseObject(std::size_t depth) {
        Expect('{');
        JsonObject members;
        SkipWhitespace();
        if (Peek() == '}') {
            ++pos_;
            return JsonValue(std::move(members));
        }
        while (true) {
            SkipWhitespace();
            if (Peek() != '"') Fail("expected object key");
            std::string key = ParseString();
            for (const JsonMember& existing : members) {
                if (existing.first == key) Fail("duplicate object key");
            }
            SkipWhitespace();
            Expect(':');
            SkipWhitespace();
            JsonValue value = ParseValue(depth + 1);
            members.emplace_back(std::move(key), std::move(value));
            SkipWhitespace();
            if (Peek() == ',') {
                ++pos_;
                continue;
            }
            Expect('}');
            return JsonValue(std::move(members));
        }
    }

    JsonValue ParseArray(std::size_t depth) {
        Expect('[');
        JsonArray items;
        SkipWhitespace();
        if (Peek() == ']') {
            ++pos_;
            return JsonValue(std::move(items));
        }
        while (true) {
            SkipWhitespace();
            items.push_back(ParseValue(depth + 1));
            SkipWhitespace();
            if (Peek() == ',') {
                ++pos_;
                continue;
            }
            Expect(']');
            return JsonValue(std::move(items));
        }
    }

    unsigned ParseHex4() {
        if (pos_ + 4 > text_.size()) Fail("truncated \\u escape");
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
            else Fail("invalid \\u escape");
        }
        return value;
    }

    std::string ParseString() {
        Expect('"');
        std::string out;
        while (true) {
            if (AtEnd()) Fail("unterminated string");
            const auto c = static_cast<unsigned char>(text_[pos_]);
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (c < 0x20) Fail("control character in string");
            if (c == '\\') {
                ++pos_;
                if (AtEnd()) Fail("unterminated escape");
                const char e = text_[pos_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        char32_t cp = ParseHex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (text_.substr(pos_, 2) != "\\u") Fail("unpaired high surrogate");
                            pos_ += 2;
                            const char32_t low = ParseHex4();
                            if (low < 0xDC00 || low > 0xDFFF) Fail("invalid low surrogate");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            Fail("unpaired low surrogate");
                        }
                        AppendUtf8(out, cp);
                        break;
                    }
                    default: Fail("invalid escape");
                }
                continue;
            }
            if (c >= 0x80) {
                char32_t cp = 0;
                const std::size_t length = DecodeUtf8(text_, pos_, cp);
                if (length == 0) Fail("invalid UTF-8");
                out.append(text_.substr(pos_, length));
                pos_ += length;
                continue;
            }
            out.push_back(static_cast<char>(c));
            ++pos_;
        }
    }

    double ParseNumber() {
        const std::size_t start = pos_;
        if (Peek() == '-') ++pos_;
        if (Peek() == '0') {
            ++pos_;
        } else if (Peek() >= '1' && Peek() <= '9') {
            while (Peek() >= '0' && Peek() <= '9') ++pos_;
        } else {
            Fail("invalid number");
        }
        if (Peek() == '.') {
            ++pos_;
            if (!(Peek() >= '0' && Peek() <= '9')) Fail("invalid fraction");
            while (Peek() >= '0' && Peek() <= '9') ++pos_;
        }
        if (Peek() == 'e' || Peek() == 'E') {
            ++pos_;
            if (Peek() == '+' || Peek() == '-') ++pos_;
            if (!(Peek() >= '0' && Peek() <= '9')) Fail("invalid exponent");
            while (Peek() >= '0' && Peek() <= '9') ++pos_;
        }
        double value = 0;
        const char* first = text_.data() + start;
        const char* last = text_.data() + pos_;
        const auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc() || ptr != last || !std::isfinite(value)) Fail("number out of range");
        return value;
    }

    std::string_view text_;
    JsonParseLimits limits_;
    std::size_t pos_ = 0;
};

}  // namespace

JsonValue ParseJson(std::string_view text, const JsonParseLimits& limits) {
    return Parser(text, limits).ParseDocument();
}

}  // namespace mwb::native

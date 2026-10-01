package dev.mobilewebcambridge.protocol.json

/**
 * Strict RFC 8259 parser for the small JSON payloads of the protocol. Recursion depth is bounded so
 * hostile input cannot exhaust the stack; duplicate keys keep the last value.
 */
class JsonParser private constructor(private val text: String) {
    private var index = 0

    private fun parseDocument(): JsonValue {
        skipWhitespace()
        val value = parseValue(depth = 0)
        skipWhitespace()
        if (index != text.length) fail("Unexpected trailing characters")
        return value
    }

    private fun parseValue(depth: Int): JsonValue {
        if (depth > MAX_DEPTH) fail("Nesting deeper than $MAX_DEPTH")
        if (index >= text.length) fail("Unexpected end of input")
        return when (text[index]) {
            '{' -> parseObject(depth)
            '[' -> parseArray(depth)
            '"' -> JsonString(parseString())
            't' -> literal("true", JsonBool(true))
            'f' -> literal("false", JsonBool(false))
            'n' -> literal("null", JsonNull)
            else -> parseNumber()
        }
    }

    private fun parseObject(depth: Int): JsonObject {
        expect('{')
        val members = LinkedHashMap<String, JsonValue>()
        skipWhitespace()
        if (peek() == '}') {
            index++
            return JsonObject(members)
        }
        while (true) {
            skipWhitespace()
            if (peek() != '"') fail("Expected a string key")
            val key = parseString()
            skipWhitespace()
            expect(':')
            skipWhitespace()
            members[key] = parseValue(depth + 1)
            skipWhitespace()
            when (next()) {
                ',' -> continue
                '}' -> return JsonObject(members)
                else -> fail("Expected ',' or '}'")
            }
        }
    }

    private fun parseArray(depth: Int): JsonArray {
        expect('[')
        val items = ArrayList<JsonValue>()
        skipWhitespace()
        if (peek() == ']') {
            index++
            return JsonArray(items)
        }
        while (true) {
            skipWhitespace()
            items += parseValue(depth + 1)
            skipWhitespace()
            when (next()) {
                ',' -> continue
                ']' -> return JsonArray(items)
                else -> fail("Expected ',' or ']'")
            }
        }
    }

    private fun parseString(): String {
        expect('"')
        val builder = StringBuilder()
        while (true) {
            if (index >= text.length) fail("Unterminated string")
            val c = text[index++]
            when {
                c == '"' -> return builder.toString()
                c == '\\' -> builder.append(parseEscape())
                c < ' ' -> fail("Unescaped control character in string")
                else -> builder.append(c)
            }
        }
    }

    private fun parseEscape(): Char {
        if (index >= text.length) fail("Unterminated escape")
        return when (val c = text[index++]) {
            '"' -> '"'
            '\\' -> '\\'
            '/' -> '/'
            'b' -> '\b'
            'f' -> '\u000C'
            'n' -> '\n'
            'r' -> '\r'
            't' -> '\t'
            'u' -> parseHexChar()
            else -> fail("Invalid escape '\\$c'")
        }
    }

    private fun parseHexChar(): Char {
        if (index + 4 > text.length) fail("Truncated \\u escape")
        var value = 0
        repeat(4) {
            val digit = Character.digit(text[index++], 16)
            if (digit < 0) fail("Invalid hex digit in \\u escape")
            value = value * 16 + digit
        }
        // Surrogate pairs arrive as two escapes; StringBuilder joins the UTF-16 units naturally.
        return value.toChar()
    }

    private fun parseNumber(): JsonValue {
        val begin = index
        if (peek() == '-') index++
        if (index >= text.length || !text[index].isAsciiDigit()) fail("Invalid number")
        if (text[index] == '0') {
            index++
        } else {
            while (index < text.length && text[index].isAsciiDigit()) index++
        }
        var integral = true
        if (index < text.length && text[index] == '.') {
            integral = false
            index++
            if (index >= text.length || !text[index].isAsciiDigit()) fail("Invalid fraction")
            while (index < text.length && text[index].isAsciiDigit()) index++
        }
        if (index < text.length && (text[index] == 'e' || text[index] == 'E')) {
            integral = false
            index++
            if (index < text.length && (text[index] == '+' || text[index] == '-')) index++
            if (index >= text.length || !text[index].isAsciiDigit()) fail("Invalid exponent")
            while (index < text.length && text[index].isAsciiDigit()) index++
        }
        val literal = text.substring(begin, index)
        if (integral) {
            literal.toLongOrNull()?.let { return JsonInt(it) }
        }
        return JsonDecimal(literal.toDouble())
    }

    private fun literal(word: String, value: JsonValue): JsonValue {
        if (!text.startsWith(word, index)) fail("Invalid literal")
        index += word.length
        return value
    }

    private fun skipWhitespace() {
        while (index < text.length) {
            when (text[index]) {
                ' ', '\t', '\n', '\r' -> index++
                else -> return
            }
        }
    }

    private fun peek(): Char? = if (index < text.length) text[index] else null

    private fun next(): Char? = if (index < text.length) text[index++] else null

    private fun expect(c: Char) {
        if (next() != c) fail("Expected '$c'")
    }

    private fun Char.isAsciiDigit(): Boolean = this in '0'..'9'

    private fun fail(message: String): Nothing = throw JsonSyntaxException(message, index)

    companion object {
        private const val MAX_DEPTH = 64

        fun parse(text: String): JsonValue = JsonParser(text).parseDocument()

        fun parse(bytes: ByteArray): JsonValue = parse(bytes.toString(Charsets.UTF_8))
    }
}

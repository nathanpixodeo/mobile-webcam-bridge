package dev.mobilewebcambridge.protocol.json

/**
 * Canonical JSON as required by SPEC §4: UTF-8, object keys sorted by Unicode code point, no
 * insignificant whitespace, no escaped forward slashes, integers only. Byte-identical to the
 * TypeScript host and the Swift package, which the golden test vectors rely on.
 */
object CanonicalJson {
    fun write(value: JsonValue): String = StringBuilder().also { append(value, it) }.toString()

    fun encode(value: JsonValue): ByteArray = write(value).toByteArray(Charsets.UTF_8)

    /** Orders strings by Unicode code point (not UTF-16 unit, not locale). */
    val codePointOrder: Comparator<String> = Comparator { left, right ->
        var i = 0
        var j = 0
        while (i < left.length && j < right.length) {
            val a = left.codePointAt(i)
            val b = right.codePointAt(j)
            if (a != b) return@Comparator a.compareTo(b)
            i += Character.charCount(a)
            j += Character.charCount(b)
        }
        (left.length - i).compareTo(right.length - j)
    }

    private fun append(value: JsonValue, out: StringBuilder) {
        when (value) {
            JsonNull -> out.append("null")
            is JsonBool -> out.append(if (value.value) "true" else "false")
            is JsonInt -> out.append(value.value)
            is JsonDecimal -> throw IllegalArgumentException("Canonical JSON allows integers only, got ${value.value}")
            is JsonString -> appendQuoted(value.value, out)
            is JsonArray -> {
                out.append('[')
                value.items.forEachIndexed { position, item ->
                    if (position > 0) out.append(',')
                    append(item, out)
                }
                out.append(']')
            }
            is JsonObject -> {
                out.append('{')
                value.members.keys.sortedWith(codePointOrder).forEachIndexed { position, key ->
                    if (position > 0) out.append(',')
                    appendQuoted(key, out)
                    out.append(':')
                    append(value.members.getValue(key), out)
                }
                out.append('}')
            }
        }
    }

    private fun appendQuoted(text: String, out: StringBuilder) {
        out.append('"')
        for (c in text) {
            when {
                c == '"' -> out.append("\\\"")
                c == '\\' -> out.append("\\\\")
                c == '\b' -> out.append("\\b")
                c == '\u000C' -> out.append("\\f")
                c == '\n' -> out.append("\\n")
                c == '\r' -> out.append("\\r")
                c == '\t' -> out.append("\\t")
                c < ' ' -> out.append("\\u").append(c.code.toString(16).padStart(4, '0'))
                else -> out.append(c)
            }
        }
        out.append('"')
    }
}

package dev.mobilewebcambridge.protocol.messages

import dev.mobilewebcambridge.protocol.json.JsonArray
import dev.mobilewebcambridge.protocol.json.JsonBool
import dev.mobilewebcambridge.protocol.json.JsonInt
import dev.mobilewebcambridge.protocol.json.JsonNull
import dev.mobilewebcambridge.protocol.json.JsonObject
import dev.mobilewebcambridge.protocol.json.JsonString
import dev.mobilewebcambridge.protocol.json.JsonValue
import dev.mobilewebcambridge.protocol.wire.PacketType

/** A packet whose framing was valid but whose content was not. */
class MessageDecodingException(
    val type: PacketType,
    val detail: String,
    val kind: Kind,
) : Exception("Invalid ${type.name}: $detail") {
    enum class Kind { INVALID_JSON, INVALID_VALUE }
}

/**
 * Typed, fail-fast access to the members of a JSON object. Missing or mistyped members are
 * reported as [MessageDecodingException.Kind.INVALID_JSON]; unknown members are ignored
 * (SPEC §1.2 forward compatibility).
 */
class JsonFields(private val json: JsonObject, private val type: PacketType) {
    fun string(key: String): String = optString(key) ?: missing(key, "string")

    fun optString(key: String): String? =
        when (val value = json[key]) {
            null, JsonNull -> null
            is JsonString -> value.value
            else -> wrongType(key, "string")
        }

    fun int(key: String): Int = optInt(key) ?: missing(key, "integer")

    fun optInt(key: String): Int? =
        when (val value = json[key]) {
            null, JsonNull -> null
            is JsonInt ->
                if (value.value in Int.MIN_VALUE.toLong()..Int.MAX_VALUE.toLong()) value.value.toInt()
                else invalid(key, "integer ${value.value} out of range")
            else -> wrongType(key, "integer")
        }

    fun bool(key: String): Boolean =
        when (val value = json[key]) {
            is JsonBool -> value.value
            null, JsonNull -> missing(key, "boolean")
            else -> wrongType(key, "boolean")
        }

    fun fields(key: String): JsonFields = optFields(key) ?: missing(key, "object")

    fun optFields(key: String): JsonFields? =
        when (val value = json[key]) {
            null, JsonNull -> null
            is JsonObject -> JsonFields(value, type)
            else -> wrongType(key, "object")
        }

    fun stringList(key: String): List<String> =
        when (val value = json[key]) {
            null, JsonNull -> emptyList()
            is JsonArray -> value.items.map { item -> (item as? JsonString)?.value ?: wrongType(key, "array of strings") }
            else -> wrongType(key, "array")
        }

    fun <E> enum(key: String, parse: (String) -> E?): E {
        val raw = string(key)
        return parse(raw) ?: invalid(key, "unsupported value \"$raw\"")
    }

    fun <E> optEnum(key: String, parse: (String) -> E?): E? {
        val raw = optString(key) ?: return null
        return parse(raw) ?: invalid(key, "unsupported value \"$raw\"")
    }

    fun invalid(key: String, detail: String): Nothing =
        throw MessageDecodingException(type, "$key: $detail", MessageDecodingException.Kind.INVALID_VALUE)

    private fun missing(key: String, expected: String): Nothing =
        throw MessageDecodingException(type, "missing $expected \"$key\"", MessageDecodingException.Kind.INVALID_JSON)

    private fun wrongType(key: String, expected: String): Nothing =
        throw MessageDecodingException(type, "\"$key\" is not a $expected", MessageDecodingException.Kind.INVALID_JSON)

    companion object {
        /** Wraps a parsed payload; anything but an object is invalid. */
        fun of(value: JsonValue, type: PacketType): JsonFields =
            (value as? JsonObject)?.let { JsonFields(it, type) }
                ?: throw MessageDecodingException(type, "payload is not a JSON object", MessageDecodingException.Kind.INVALID_JSON)
    }
}

package dev.mobilewebcambridge.protocol.json

/**
 * Minimal JSON document model. Integers are kept exact as [Long]; any other number parses as
 * [JsonDecimal] (the protocol never sends one, but unknown fields of a newer peer may).
 */
sealed interface JsonValue

data object JsonNull : JsonValue

data class JsonBool(val value: Boolean) : JsonValue

data class JsonInt(val value: Long) : JsonValue

data class JsonDecimal(val value: Double) : JsonValue

data class JsonString(val value: String) : JsonValue

data class JsonArray(val items: List<JsonValue>) : JsonValue

data class JsonObject(val members: Map<String, JsonValue>) : JsonValue {
    operator fun get(key: String): JsonValue? = members[key]
}

/** Builds a [JsonObject] skipping null values (optional fields are omitted, never `null`). */
fun jsonObjectOf(vararg entries: Pair<String, JsonValue?>): JsonObject {
    val members = LinkedHashMap<String, JsonValue>(entries.size)
    for ((key, value) in entries) if (value != null) members[key] = value
    return JsonObject(members)
}

fun Int.toJson(): JsonValue = JsonInt(toLong())

fun Long.toJson(): JsonValue = JsonInt(this)

fun Boolean.toJson(): JsonValue = JsonBool(this)

fun String.toJson(): JsonValue = JsonString(this)

/** Raised for malformed JSON text. */
class JsonSyntaxException(message: String, val position: Int) : Exception("$message at offset $position")

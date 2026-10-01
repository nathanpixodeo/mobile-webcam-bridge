package dev.mobilewebcambridge.protocol

import dev.mobilewebcambridge.protocol.json.JsonArray
import dev.mobilewebcambridge.protocol.json.JsonBool
import dev.mobilewebcambridge.protocol.json.JsonInt
import dev.mobilewebcambridge.protocol.json.JsonObject
import dev.mobilewebcambridge.protocol.json.JsonParser
import dev.mobilewebcambridge.protocol.json.JsonString
import dev.mobilewebcambridge.protocol.json.JsonValue
import java.io.File

/**
 * Loads the golden vectors shared with the host and iOS implementations (protocol/test-vectors).
 * Gradle passes the repository root as `mwb.repoRoot`; running from an IDE falls back to walking
 * up from the working directory.
 */
object TestVectors {
    private val directory: File by lazy {
        val configured = System.getProperty("mwb.repoRoot")?.let { File(it, "protocol/test-vectors") }
        if (configured != null && configured.isDirectory) return@lazy configured
        var current: File? = File(System.getProperty("user.dir")).absoluteFile
        while (current != null) {
            val candidate = File(current, "protocol/test-vectors")
            if (candidate.isDirectory) return@lazy candidate
            current = current.parentFile
        }
        error("protocol/test-vectors not found; set the mwb.repoRoot system property")
    }

    fun load(name: String): JsonObject = JsonParser.parse(File(directory, name).readText()) as JsonObject

    val packets: JsonObject by lazy { load("packets.json") }
    val h264: JsonObject by lazy { load("h264.json") }
}

fun hex(text: String): ByteArray {
    require(text.length % 2 == 0) { "odd hex length" }
    return ByteArray(text.length / 2) { i -> text.substring(2 * i, 2 * i + 2).toInt(16).toByte() }
}

fun ByteArray.toHex(): String = joinToString("") { "%02x".format(it.toInt() and 0xFF) }

// Terse accessors for vector documents.
fun JsonValue.obj(): JsonObject = this as JsonObject

fun JsonValue.array(): List<JsonValue> = (this as JsonArray).items

fun JsonObject.str(key: String): String = (this[key] as JsonString).value

fun JsonObject.long(key: String): Long = (this[key] as JsonInt).value

fun JsonObject.int(key: String): Int = long(key).toInt()

fun JsonObject.bool(key: String): Boolean = (this[key] as JsonBool).value

fun JsonObject.objects(key: String): List<JsonObject> = (this[key] as JsonArray).items.map { it as JsonObject }

/** Deterministic PRNG (mulberry32), identical to the host's fuzzing helper. */
class SeededRandom(seed: Int) {
    private var state: Int = seed

    fun next(): Double {
        state += 0x6D2B79F5
        var t = state
        t = (t xor (t ushr 15)) * (t or 1)
        t = t xor (t + (t xor (t ushr 7)) * (t or 61))
        return ((t xor (t ushr 14)).toLong() and 0xFFFF_FFFFL).toDouble() / 4_294_967_296.0
    }
}

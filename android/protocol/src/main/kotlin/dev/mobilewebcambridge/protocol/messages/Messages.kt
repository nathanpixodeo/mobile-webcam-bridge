package dev.mobilewebcambridge.protocol.messages

import dev.mobilewebcambridge.protocol.json.JsonArray
import dev.mobilewebcambridge.protocol.json.JsonObject
import dev.mobilewebcambridge.protocol.json.JsonString
import dev.mobilewebcambridge.protocol.json.jsonObjectOf
import dev.mobilewebcambridge.protocol.json.toJson
import dev.mobilewebcambridge.protocol.wire.PacketType
import dev.mobilewebcambridge.protocol.wire.ProtocolException
import dev.mobilewebcambridge.protocol.wire.ProtocolLimits
import java.nio.ByteBuffer
import java.nio.ByteOrder

// JSON payloads of SPEC §4. Every message maps to and from the JSON model; `CanonicalJson`
// produces the wire bytes. Optional fields are omitted when absent, never written as null.

/** A message with a JSON payload. */
interface JsonMessage {
    fun toJson(): JsonObject
}

data class ProtocolVersion(val major: Int, val minor: Int) : JsonMessage {
    override fun toJson(): JsonObject = jsonObjectOf("major" to major.toJson(), "minor" to minor.toJson())

    companion object {
        val CURRENT: ProtocolVersion = ProtocolVersion(major = 1, minor = 0)

        fun from(fields: JsonFields): ProtocolVersion = ProtocolVersion(fields.int("major"), fields.int("minor"))
    }
}

data class AppDescriptor(
    val name: String,
    val version: String,
    val build: String,
    val gitSha: String,
) : JsonMessage {
    override fun toJson(): JsonObject = jsonObjectOf(
        "name" to name.toJson(),
        "version" to version.toJson(),
        "build" to build.toJson(),
        "gitSha" to gitSha.toJson(),
    )

    companion object {
        fun from(fields: JsonFields): AppDescriptor =
            AppDescriptor(fields.string("name"), fields.string("version"), fields.string("build"), fields.string("gitSha"))
    }
}

data class DeviceDescriptor(val model: String, val name: String, val os: String) : JsonMessage {
    override fun toJson(): JsonObject =
        jsonObjectOf("model" to model.toJson(), "name" to name.toJson(), "os" to os.toJson())

    companion object {
        fun from(fields: JsonFields): DeviceDescriptor =
            DeviceDescriptor(fields.string("model"), fields.string("name"), fields.string("os"))
    }
}

data class HelloMessage(
    val protocolVersion: ProtocolVersion = ProtocolVersion.CURRENT,
    val role: PeerRole,
    val app: AppDescriptor,
    val device: DeviceDescriptor? = null,
    val features: List<String>,
) : JsonMessage {
    override fun toJson(): JsonObject = jsonObjectOf(
        "protocol" to protocolVersion.toJson(),
        "role" to role.wire.toJson(),
        "app" to app.toJson(),
        "device" to device?.toJson(),
        "features" to JsonArray(features.sorted().map { JsonString(it) }),
    )

    companion object {
        fun from(fields: JsonFields): HelloMessage = HelloMessage(
            protocolVersion = ProtocolVersion.from(fields.fields("protocol")),
            role = fields.enum("role") { PeerRole.fromWire(it) },
            app = AppDescriptor.from(fields.fields("app")),
            device = fields.optFields("device")?.let { DeviceDescriptor.from(it) },
            features = fields.stringList("features"),
        )
    }
}

/** Extensible error code: unknown codes from a newer peer still decode. */
@JvmInline
value class ErrorCode(val raw: String) {
    companion object {
        val VERSION_MISMATCH: ErrorCode = ErrorCode("VERSION_MISMATCH")
        val BAD_REQUEST: ErrorCode = ErrorCode("BAD_REQUEST")
        val PERMISSION_DENIED: ErrorCode = ErrorCode("PERMISSION_DENIED")
        val CAMERA_UNAVAILABLE: ErrorCode = ErrorCode("CAMERA_UNAVAILABLE")
        val MIC_UNAVAILABLE: ErrorCode = ErrorCode("MIC_UNAVAILABLE")
        val ENCODER_FAILURE: ErrorCode = ErrorCode("ENCODER_FAILURE")
        val INTERNAL: ErrorCode = ErrorCode("INTERNAL")
    }
}

data class ErrorMessage(val code: ErrorCode, val message: String, val fatal: Boolean) : JsonMessage {
    override fun toJson(): JsonObject =
        jsonObjectOf("code" to code.raw.toJson(), "message" to message.toJson(), "fatal" to fatal.toJson())

    companion object {
        fun from(fields: JsonFields): ErrorMessage =
            ErrorMessage(ErrorCode(fields.string("code")), fields.string("message"), fields.bool("fatal"))
    }
}

data class StartVideoMessage(
    val width: Int,
    val height: Int,
    val fps: Int,
    val bitrateKbps: Int,
    val camera: CameraSelection,
    val mirror: Boolean,
    val orientation: OrientationMode,
    val encoder: EncoderMode,
) : JsonMessage {
    override fun toJson(): JsonObject = jsonObjectOf(
        "width" to width.toJson(),
        "height" to height.toJson(),
        "fps" to fps.toJson(),
        "bitrateKbps" to bitrateKbps.toJson(),
        "camera" to camera.wire.toJson(),
        "mirror" to mirror.toJson(),
        "orientation" to orientation.wire.toJson(),
        "encoder" to encoder.wire.toJson(),
    )

    companion object {
        fun from(fields: JsonFields): StartVideoMessage {
            val message = StartVideoMessage(
                width = fields.int("width"),
                height = fields.int("height"),
                fps = fields.int("fps"),
                bitrateKbps = fields.int("bitrateKbps"),
                camera = fields.enum("camera") { CameraSelection.fromWire(it) },
                mirror = fields.bool("mirror"),
                orientation = fields.enum("orientation") { OrientationMode.fromWire(it) },
                encoder = fields.enum("encoder") { EncoderMode.fromWire(it) },
            )
            // Range checks from SPEC §4 `StartVideo` (sizes in either orientation).
            if (message.width !in 160..3840 || message.height !in 120..3840) {
                fields.invalid("width/height", "size ${message.width}x${message.height} out of range")
            }
            if (message.fps !in 15..60) fields.invalid("fps", "${message.fps} out of range 15-60")
            if (message.bitrateKbps !in 500..40_000) fields.invalid("bitrateKbps", "${message.bitrateKbps} out of range 500-40000")
            return message
        }
    }
}

data class StartAudioMessage(val processing: AudioProcessing) : JsonMessage {
    override fun toJson(): JsonObject = jsonObjectOf("processing" to processing.wire.toJson())

    companion object {
        fun from(fields: JsonFields): StartAudioMessage = StartAudioMessage(fields.enum("processing") { AudioProcessing.fromWire(it) })
    }
}

data class VideoConfigMessage(
    val configId: Int,
    val codec: String = "h264",
    val profile: String,
    val width: Int,
    val height: Int,
    val fps: Int,
    val bitrateKbps: Int,
    val rotationDeg: Int,
    val mirrored: Boolean,
    val encoder: EncoderMode,
    val camera: CameraSelection,
) : JsonMessage {
    override fun toJson(): JsonObject = jsonObjectOf(
        "configId" to configId.toJson(),
        "codec" to codec.toJson(),
        "profile" to profile.toJson(),
        "width" to width.toJson(),
        "height" to height.toJson(),
        "fps" to fps.toJson(),
        "bitrateKbps" to bitrateKbps.toJson(),
        "rotationDeg" to rotationDeg.toJson(),
        "mirrored" to mirrored.toJson(),
        "encoder" to encoder.wire.toJson(),
        "camera" to camera.wire.toJson(),
    )

    companion object {
        fun from(fields: JsonFields): VideoConfigMessage = VideoConfigMessage(
            configId = fields.int("configId"),
            codec = fields.string("codec"),
            profile = fields.string("profile"),
            width = fields.int("width"),
            height = fields.int("height"),
            fps = fields.int("fps"),
            bitrateKbps = fields.int("bitrateKbps"),
            rotationDeg = fields.int("rotationDeg"),
            mirrored = fields.bool("mirrored"),
            encoder = fields.enum("encoder") { EncoderMode.fromWire(it) },
            camera = fields.enum("camera") { CameraSelection.fromWire(it) },
        )
    }
}

data class AudioConfigMessage(
    val configId: Int,
    val format: String = "s16le",
    val sampleRate: Int = 48_000,
    val channels: Int = 1,
) : JsonMessage {
    override fun toJson(): JsonObject = jsonObjectOf(
        "configId" to configId.toJson(),
        "format" to format.toJson(),
        "sampleRate" to sampleRate.toJson(),
        "channels" to channels.toJson(),
    )

    companion object {
        fun from(fields: JsonFields): AudioConfigMessage = AudioConfigMessage(
            configId = fields.int("configId"),
            format = fields.string("format"),
            sampleRate = fields.int("sampleRate"),
            channels = fields.int("channels"),
        )
    }
}

data class VideoStreamStatus(
    val state: StreamState,
    val reason: String? = null,
    val width: Int? = null,
    val height: Int? = null,
    val fps: Int? = null,
    val bitrateKbps: Int? = null,
) : JsonMessage {
    override fun toJson(): JsonObject = jsonObjectOf(
        "state" to state.wire.toJson(),
        "reason" to reason?.toJson(),
        "width" to width?.toJson(),
        "height" to height?.toJson(),
        "fps" to fps?.toJson(),
        "bitrateKbps" to bitrateKbps?.toJson(),
    )

    companion object {
        val OFF: VideoStreamStatus = VideoStreamStatus(StreamState.OFF)

        fun from(fields: JsonFields): VideoStreamStatus = VideoStreamStatus(
            state = fields.enum("state") { StreamState.fromWire(it) },
            reason = fields.optString("reason"),
            width = fields.optInt("width"),
            height = fields.optInt("height"),
            fps = fields.optInt("fps"),
            bitrateKbps = fields.optInt("bitrateKbps"),
        )
    }
}

data class AudioStreamStatus(val state: StreamState, val reason: String? = null) : JsonMessage {
    override fun toJson(): JsonObject = jsonObjectOf("state" to state.wire.toJson(), "reason" to reason?.toJson())

    companion object {
        val OFF: AudioStreamStatus = AudioStreamStatus(StreamState.OFF)

        fun from(fields: JsonFields): AudioStreamStatus =
            AudioStreamStatus(fields.enum("state") { StreamState.fromWire(it) }, fields.optString("reason"))
    }
}

data class BatteryStatus(val levelPercent: Int, val charging: Boolean) : JsonMessage {
    override fun toJson(): JsonObject =
        jsonObjectOf("levelPercent" to levelPercent.toJson(), "charging" to charging.toJson())

    companion object {
        fun from(fields: JsonFields): BatteryStatus = BatteryStatus(fields.int("levelPercent"), fields.bool("charging"))
    }
}

data class PermissionsStatus(val camera: PermissionState, val microphone: PermissionState) : JsonMessage {
    override fun toJson(): JsonObject =
        jsonObjectOf("camera" to camera.wire.toJson(), "microphone" to microphone.wire.toJson())

    companion object {
        fun from(fields: JsonFields): PermissionsStatus = PermissionsStatus(
            fields.enum("camera") { PermissionState.fromWire(it) },
            fields.enum("microphone") { PermissionState.fromWire(it) },
        )
    }
}

data class StatusMessage(
    val appState: AppState,
    val audio: AudioStreamStatus,
    val battery: BatteryStatus?,
    val lowPower: Boolean,
    val permissions: PermissionsStatus,
    val thermal: ThermalStateName,
    val video: VideoStreamStatus,
) : JsonMessage {
    override fun toJson(): JsonObject = jsonObjectOf(
        "appState" to appState.wire.toJson(),
        "audio" to audio.toJson(),
        "battery" to battery?.toJson(),
        "lowPower" to lowPower.toJson(),
        "permissions" to permissions.toJson(),
        "thermal" to thermal.wire.toJson(),
        "video" to video.toJson(),
    )

    companion object {
        fun from(fields: JsonFields): StatusMessage = StatusMessage(
            appState = fields.enum("appState") { AppState.fromWire(it) },
            audio = AudioStreamStatus.from(fields.fields("audio")),
            battery = fields.optFields("battery")?.let { BatteryStatus.from(it) },
            lowPower = fields.bool("lowPower"),
            permissions = PermissionsStatus.from(fields.fields("permissions")),
            thermal = fields.enum("thermal") { ThermalStateName.fromWire(it) },
            video = VideoStreamStatus.from(fields.fields("video")),
        )
    }
}

data class LogMessage(val level: LogLevel, val category: String, val message: String) : JsonMessage {
    override fun toJson(): JsonObject = jsonObjectOf(
        "level" to level.wire.toJson(),
        "category" to category.toJson(),
        "message" to message.toJson(),
    )

    /**
     * Cuts [message] on a code point boundary so the encoded payload stays well under the 8 KiB
     * `Log` limit even after JSON escaping.
     */
    fun truncated(maxMessageBytes: Int = 6_000): LogMessage {
        if (message.toByteArray(Charsets.UTF_8).size <= maxMessageBytes) return this
        val builder = StringBuilder()
        var bytes = 0
        var index = 0
        while (index < message.length) {
            val codePoint = message.codePointAt(index)
            val size = String(Character.toChars(codePoint)).toByteArray(Charsets.UTF_8).size
            if (bytes + size > maxMessageBytes) break
            builder.appendCodePoint(codePoint)
            bytes += size
            index += Character.charCount(codePoint)
        }
        return copy(message = builder.append('…').toString())
    }

    companion object {
        fun from(fields: JsonFields): LogMessage =
            LogMessage(fields.enum("level") { LogLevel.fromWire(it) }, fields.string("category"), fields.string("message"))
    }
}

/** `Pong` binary payload: `echoT0` and `t1`, big-endian i64 each (SPEC §3.1). */
data class PongPayload(val echoT0: Long, val t1: Long) {
    fun encode(): ByteArray =
        ByteBuffer.allocate(ProtocolLimits.PONG_PAYLOAD_LENGTH.toInt())
            .order(ByteOrder.BIG_ENDIAN)
            .putLong(echoT0)
            .putLong(t1)
            .array()

    companion object {
        fun decode(payload: ByteArray): PongPayload {
            if (payload.size.toLong() != ProtocolLimits.PONG_PAYLOAD_LENGTH) {
                throw ProtocolException.BadPayloadLength(PacketType.PONG.code, payload.size.toLong())
            }
            val buffer = ByteBuffer.wrap(payload).order(ByteOrder.BIG_ENDIAN)
            return PongPayload(echoT0 = buffer.long, t1 = buffer.long)
        }
    }
}

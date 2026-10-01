package dev.mobilewebcambridge.protocol.messages

// Wire enums of SPEC §4. Each constant carries its exact wire spelling.

enum class PeerRole(val wire: String) {
    HOST("host"),
    DEVICE("device"),
    ;

    companion object {
        fun fromWire(value: String): PeerRole? = entries.firstOrNull { it.wire == value }
    }
}

enum class CameraSelection(val wire: String) {
    BACK_WIDE("back.wide"),
    BACK_ULTRA_WIDE("back.ultraWide"),
    BACK_TELEPHOTO("back.telephoto"),
    FRONT("front"),
    ;

    companion object {
        fun fromWire(value: String): CameraSelection? = entries.firstOrNull { it.wire == value }
    }
}

enum class OrientationMode(val wire: String) {
    AUTO("auto"),
    LANDSCAPE("landscape"),
    PORTRAIT("portrait"),
    ;

    companion object {
        fun fromWire(value: String): OrientationMode? = entries.firstOrNull { it.wire == value }
    }
}

enum class EncoderMode(val wire: String) {
    LOW_LATENCY("lowLatency"),
    STANDARD("standard"),
    ;

    companion object {
        fun fromWire(value: String): EncoderMode? = entries.firstOrNull { it.wire == value }
    }
}

enum class AudioProcessing(val wire: String) {
    STANDARD("standard"),
    RAW("raw"),
    VOICE("voice"),
    ;

    companion object {
        fun fromWire(value: String): AudioProcessing? = entries.firstOrNull { it.wire == value }
    }
}

enum class StreamState(val wire: String) {
    OFF("off"),
    STARTING("starting"),
    RUNNING("running"),
    INTERRUPTED("interrupted"),
    ERROR("error"),
    ;

    companion object {
        fun fromWire(value: String): StreamState? = entries.firstOrNull { it.wire == value }
    }
}

enum class PermissionState(val wire: String) {
    AUTHORIZED("authorized"),
    DENIED("denied"),
    RESTRICTED("restricted"),
    NOT_DETERMINED("notDetermined"),
    ;

    companion object {
        fun fromWire(value: String): PermissionState? = entries.firstOrNull { it.wire == value }
    }
}

enum class ThermalStateName(val wire: String) {
    NOMINAL("nominal"),
    FAIR("fair"),
    SERIOUS("serious"),
    CRITICAL("critical"),
    ;

    companion object {
        fun fromWire(value: String): ThermalStateName? = entries.firstOrNull { it.wire == value }
    }
}

enum class AppState(val wire: String) {
    ACTIVE("active"),
    INACTIVE("inactive"),
    BACKGROUND("background"),
    ;

    companion object {
        fun fromWire(value: String): AppState? = entries.firstOrNull { it.wire == value }
    }
}

/** Log severity; declaration order is severity order. */
enum class LogLevel(val wire: String) {
    DEBUG("debug"),
    INFO("info"),
    WARN("warn"),
    ERROR("error"),
    ;

    companion object {
        fun fromWire(value: String): LogLevel? = entries.firstOrNull { it.wire == value }
    }
}

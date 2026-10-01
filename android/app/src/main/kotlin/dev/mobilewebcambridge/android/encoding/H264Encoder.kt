package dev.mobilewebcambridge.android.encoding

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Process
import android.view.Surface
import dev.mobilewebcambridge.android.capture.CaptureException
import dev.mobilewebcambridge.android.core.AppLogger
import dev.mobilewebcambridge.android.core.SerialThread
import dev.mobilewebcambridge.protocol.media.AnnexB
import dev.mobilewebcambridge.protocol.media.EncodedVideoFrame
import dev.mobilewebcambridge.protocol.messages.EncoderMode
import java.util.concurrent.atomic.AtomicInteger

/**
 * Hardware H.264 encoder fed through its input surface, in MediaCodec's asynchronous mode.
 * Callbacks run on the encoder's own thread.
 */
class H264Encoder private constructor(
    override val configId: Int,
    override val settings: EncoderSettings,
    override val profileName: String,
    override val effectiveMode: EncoderMode,
    private val codec: MediaCodec,
    private val thread: SerialThread,
    private val outputs: OutputHandler,
    private val logger: AppLogger,
) : VideoEncoder {
    // createInputSurface() is only legal between configure() and start(): the factory constructs
    // this object exactly there.
    override val inputSurface: Surface = codec.createInputSurface()

    override fun requestSyncFrame() {
        if (outputs.released) return
        runCatching { codec.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) }) }
            .onFailure { logger.warn("encoder", "Sync frame request failed: ${it.message}") }
    }

    override fun reconfigure(configId: Int, bitrateKbps: Int) {
        if (outputs.released) return
        outputs.pendingConfigId.set(configId)
        runCatching {
            codec.setParameters(
                Bundle().apply {
                    putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bitrateKbps * 1_000)
                    putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
                },
            )
        }.onFailure { logger.warn("encoder", "Live reconfiguration failed: ${it.message}") }
    }

    override fun release() {
        if (outputs.released) return
        outputs.released = true
        runCatching { codec.stop() }
        runCatching { codec.release() }
        inputSurface.release()
        thread.quit()
    }

    /**
     * Turns encoder output buffers into wire-ready access units: 4-byte start codes, SPS/PPS on
     * every IDR (from the last codec-config buffer when the encoder does not repeat them), no AUD.
     */
    private class OutputHandler(
        private var configId: Int, // encoder thread
        private val logger: AppLogger,
        private val output: (EncodedVideoFrame) -> Unit,
        private val failure: (String) -> Unit,
    ) : MediaCodec.Callback() {
        @Volatile
        var released = false

        /** Configuration to switch to at the next IDR; 0 = none (ids start at 1). */
        val pendingConfigId = AtomicInteger(0)
        private var parameterSets: ByteArray? = null

        override fun onInputBufferAvailable(codec: MediaCodec, index: Int) = Unit // surface input

        override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
            if (released) return
            // An exception escaping a HandlerThread kills the app; report it instead. The codec
            // may also be stopped by release() while this callback is already running.
            try {
                val bytes = copyAndRelease(codec, index, info) ?: return
                if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                    parameterSets = bytes
                    return
                }
                val annexB = AnnexB.normalizeAccessUnit(bytes, parameterSets)
                val keyframe = (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0 || AnnexB.containsIdr(annexB)
                if (keyframe) {
                    // A pending configuration starts here: its first access unit is this IDR.
                    val next = pendingConfigId.getAndSet(0)
                    if (next != 0) configId = next
                }
                output(EncodedVideoFrame.fromAnnexB(configId, annexB, keyframe, info.presentationTimeUs))
            } catch (error: Exception) {
                if (!released) failure("Encoder output failed: ${error.message}")
            }
        }

        private fun copyAndRelease(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo): ByteArray? {
            try {
                val buffer = codec.getOutputBuffer(index) ?: return null
                if (info.size <= 0) return null
                buffer.position(info.offset)
                buffer.limit(info.offset + info.size)
                return ByteArray(info.size).also { buffer.get(it) }
            } finally {
                codec.releaseOutputBuffer(index, false)
            }
        }

        override fun onError(codec: MediaCodec, error: MediaCodec.CodecException) {
            if (released) return
            failure("MediaCodec error ${error.diagnosticInfo} (recoverable=${error.isRecoverable}, transient=${error.isTransient})")
        }

        override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
            logger.debug("encoder", "Output format: $format")
        }
    }

    /** Creates hardware H.264 encoders configured for low latency. */
    class Factory(private val logger: AppLogger) : VideoEncoderFactory {
        override fun create(
            configId: Int,
            settings: EncoderSettings,
            output: (EncodedVideoFrame) -> Unit,
            failure: (String) -> Unit,
        ): VideoEncoder {
            val info = findEncoder() ?: throw CaptureException.encoderFailure("No H.264 encoder on this device")
            val capabilities = info.getCapabilitiesForType(MIME)
            val video = capabilities.videoCapabilities
            if (video != null && !video.isSizeSupported(settings.width, settings.height)) {
                throw CaptureException.encoderFailure("${info.name} cannot encode ${settings.width}x${settings.height}")
            }
            val profile = EncoderProfiles.choose(capabilities.profileLevels.map { it.profile to it.level })
            val tuned = buildFormat(settings, capabilities, profile)
            val encoder = try {
                open(info, configId, settings, tuned, profile?.wireName ?: "high", settings.mode, output, failure)
            } catch (error: Exception) {
                // Some encoders refuse the optional keys (realtime priority, operating rate, latency,
                // profile): fall back to a plain configuration rather than no video at all.
                logger.warn("encoder", "${info.name} rejected the tuned configuration (${error.message}); using defaults")
                try {
                    open(info, configId, settings, basicFormat(settings), "baseline", EncoderMode.STANDARD, output, failure)
                } catch (fallbackError: Exception) {
                    throw CaptureException.encoderFailure("Cannot configure ${info.name}: ${fallbackError.message}")
                }
            }
            logger.info(
                "encoder",
                "${info.name} ${settings.width}x${settings.height}@${settings.fps} ${settings.bitrateKbps} kbps " +
                    "${encoder.profileName} ${encoder.effectiveMode.wire}",
            )
            return encoder
        }

        /** Creates, configures and starts one codec; on failure everything is released again. */
        private fun open(
            info: MediaCodecInfo,
            configId: Int,
            settings: EncoderSettings,
            format: MediaFormat,
            profileName: String,
            mode: EncoderMode,
            output: (EncodedVideoFrame) -> Unit,
            failure: (String) -> Unit,
        ): H264Encoder {
            val outputs = OutputHandler(configId, logger, output, failure)
            val thread = SerialThread("mwb-encoder", Process.THREAD_PRIORITY_URGENT_DISPLAY)
            var codec: MediaCodec? = null
            try {
                val created = MediaCodec.createByCodecName(info.name)
                codec = created
                return thread.call {
                    created.setCallback(outputs, thread.handler)
                    created.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                    val encoder = H264Encoder(configId, settings, profileName, mode, created, thread, outputs, logger)
                    created.start()
                    encoder
                }
            } catch (error: Exception) {
                outputs.released = true
                codec?.let { runCatching { it.release() } }
                thread.quit()
                throw error
            }
        }

        private fun findEncoder(): MediaCodecInfo? {
            val candidates = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
                .filter { it.isEncoder && it.supportedTypes.any { type -> type.equals(MIME, ignoreCase = true) } }
            return candidates.firstOrNull { it.isHardwareAccelerated && !it.isSoftwareOnly } ?: candidates.firstOrNull()
        }

        private fun buildFormat(
            settings: EncoderSettings,
            capabilities: MediaCodecInfo.CodecCapabilities,
            profile: EncoderProfiles.Choice?,
        ): MediaFormat = MediaFormat.createVideoFormat(MIME, settings.width, settings.height).apply {
            val lowLatency = settings.mode == EncoderMode.LOW_LATENCY
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, settings.bitrateKbps * 1_000)
            setInteger(MediaFormat.KEY_FRAME_RATE, settings.fps)
            // Long GOP: the host asks for an IDR whenever it needs one (decoder start, loss).
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, if (lowLatency) 10 else 2)
            val encoderCapabilities = capabilities.encoderCapabilities
            val cbr = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR
            val vbr = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR
            if (lowLatency && encoderCapabilities?.isBitrateModeSupported(cbr) == true) {
                setInteger(MediaFormat.KEY_BITRATE_MODE, cbr)
            } else if (encoderCapabilities?.isBitrateModeSupported(vbr) == true) {
                setInteger(MediaFormat.KEY_BITRATE_MODE, vbr)
            }
            if (profile != null) {
                setInteger(MediaFormat.KEY_PROFILE, profile.profile)
                setInteger(MediaFormat.KEY_LEVEL, profile.level)
            }
            setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1)
            setInteger(MediaFormat.KEY_PRIORITY, 0) // realtime
            setInteger(MediaFormat.KEY_OPERATING_RATE, settings.fps)
            if (lowLatency) {
                // The encoder-side latency knob: emit each frame as soon as it is encoded.
                setInteger(MediaFormat.KEY_LATENCY, 1)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
                    capabilities.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)
                ) {
                    setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
                }
            }
            setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709)
            setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
            setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO)
        }

        /** Only the keys every surface-input encoder understands. */
        private fun basicFormat(settings: EncoderSettings): MediaFormat =
            MediaFormat.createVideoFormat(MIME, settings.width, settings.height).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                setInteger(MediaFormat.KEY_BIT_RATE, settings.bitrateKbps * 1_000)
                setInteger(MediaFormat.KEY_FRAME_RATE, settings.fps)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2)
            }
    }

    private companion object {
        const val MIME = MediaFormat.MIMETYPE_VIDEO_AVC
    }
}

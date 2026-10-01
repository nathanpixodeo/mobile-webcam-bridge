package dev.mobilewebcambridge.protocol.media

import dev.mobilewebcambridge.protocol.wire.VideoFlags

/** One encoded H.264 access unit, already in wire (Annex-B) form, ready to cross threads. */
class EncodedVideoFrame(
    /** Identifies the `VideoConfig` this frame belongs to; stale frames are dropped. */
    val configId: Int,
    val annexB: ByteArray,
    val isKeyframe: Boolean,
    val hasParameterSets: Boolean,
    /** Not referenced by any other frame, so it can be dropped without breaking decoding. */
    val isDisposable: Boolean,
    /** Presentation time on the device monotonic clock, in microseconds. */
    val presentationTimeUs: Long,
) {
    val flags: Int
        get() {
            var flags = 0
            if (isKeyframe) flags = flags or VideoFlags.IDR
            if (hasParameterSets) flags = flags or VideoFlags.PARAMETER_SETS
            if (isDisposable) flags = flags or VideoFlags.DISPOSABLE
            return flags
        }

    companion object {
        /** Builds a frame from normalised Annex-B, deriving the flags from the NAL units. */
        fun fromAnnexB(configId: Int, annexB: ByteArray, encoderKeyframe: Boolean, presentationTimeUs: Long): EncodedVideoFrame {
            val isKeyframe = encoderKeyframe || AnnexB.containsIdr(annexB)
            return EncodedVideoFrame(
                configId = configId,
                annexB = annexB,
                isKeyframe = isKeyframe,
                hasParameterSets = AnnexB.containsParameterSets(annexB),
                isDisposable = !isKeyframe && AnnexB.isDisposable(annexB),
                presentationTimeUs = presentationTimeUs,
            )
        }
    }
}

/** A chunk of PCM `s16le`, 48 kHz, mono (SPEC §3.4). */
class AudioChunk(
    val configId: Int,
    val pcm: ByteArray,
    /** Capture time of the first sample on the device monotonic clock, in microseconds. */
    val timestampUs: Long,
    val isDiscontinuity: Boolean,
) {
    val sampleCount: Int get() = pcm.size / BYTES_PER_SAMPLE

    val durationUs: Long get() = sampleCount.toLong() * 1_000_000L / SAMPLE_RATE

    companion object {
        const val SAMPLE_RATE: Int = 48_000
        const val BYTES_PER_SAMPLE: Int = 2

        /** 40 ms, the largest chunk the spec allows. */
        const val MAX_SAMPLES_PER_CHUNK: Int = 1_920

        /** Splits PCM into chunks of at most 40 ms; only the first keeps the discontinuity flag. */
        fun split(pcm: ByteArray, configId: Int, timestampUs: Long, isDiscontinuity: Boolean): List<AudioChunk> {
            val maxBytes = MAX_SAMPLES_PER_CHUNK * BYTES_PER_SAMPLE
            val chunks = ArrayList<AudioChunk>()
            var offset = 0
            var samplesBefore = 0L
            while (offset < pcm.size) {
                val end = minOf(offset + maxBytes, pcm.size)
                val slice = pcm.copyOfRange(offset, end)
                val timestamp = timestampUs + samplesBefore * 1_000_000L / SAMPLE_RATE
                chunks += AudioChunk(configId, slice, timestamp, isDiscontinuity && chunks.isEmpty())
                samplesBefore += (slice.size / BYTES_PER_SAMPLE).toLong()
                offset = end
            }
            return chunks
        }
    }
}

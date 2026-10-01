package dev.mobilewebcambridge.android.capture

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.AudioRecordingConfiguration
import android.media.AudioTimestamp
import android.media.MediaRecorder
import android.os.Process
import android.os.SystemClock
import androidx.core.content.ContextCompat
import dev.mobilewebcambridge.android.core.AppLogger
import dev.mobilewebcambridge.protocol.media.AudioChunk
import dev.mobilewebcambridge.protocol.media.Pcm16
import dev.mobilewebcambridge.protocol.messages.AudioProcessing
import dev.mobilewebcambridge.protocol.messages.StartAudioMessage
import java.util.concurrent.Executors
import kotlin.math.abs

/**
 * The phone microphone through `AudioRecord`: 48 kHz mono PCM16 read in 20 ms blocks on a
 * dedicated thread, time-stamped on `CLOCK_BOOTTIME` (the clock video uses).
 *
 * The `processing` value picks the audio source (SPEC §4 `StartAudio`). When another app takes
 * priority (a call, a voice assistant) Android silences this recording instead of stopping it;
 * that is reported as an interruption with reason `silenced`.
 */
class MicAudioSource(private val context: Context, private val logger: AppLogger) : AudioSampleSource {
    private val audioManager: AudioManager = context.getSystemService(AudioManager::class.java)
    private var activeRun: RecordRun? = null

    override fun start(request: StartAudioMessage, listener: AudioSourceListener) {
        stop()
        if (ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            throw CaptureException.permissionDenied("Microphone permission has not been granted")
        }
        val (source, sourceName) = audioSource(request.processing)
        val record = createRecord(source)
        logger.info("audio", "Microphone source $sourceName, buffer ${record.bufferSizeInFrames} frames")
        val recording = RecordRun(record, listener)
        try {
            recording.start()
        } catch (error: Exception) {
            recording.stop()
            throw CaptureException.micUnavailable("Cannot start recording: ${error.message}")
        }
        activeRun = recording
    }

    override fun stop() {
        activeRun?.stop()
        activeRun = null
    }

    private fun audioSource(processing: AudioProcessing): Pair<Int, String> = when (processing) {
        AudioProcessing.STANDARD -> MediaRecorder.AudioSource.CAMCORDER to "CAMCORDER"
        AudioProcessing.VOICE -> MediaRecorder.AudioSource.VOICE_COMMUNICATION to "VOICE_COMMUNICATION"
        AudioProcessing.RAW ->
            if (audioManager.getProperty(AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED) == "true") {
                MediaRecorder.AudioSource.UNPROCESSED to "UNPROCESSED"
            } else {
                MediaRecorder.AudioSource.VOICE_RECOGNITION to "VOICE_RECOGNITION"
            }
    }

    @SuppressLint("MissingPermission") // checked in start()
    private fun createRecord(source: Int): AudioRecord {
        val minBuffer = AudioRecord.getMinBufferSize(AudioChunk.SAMPLE_RATE, CHANNEL_MASK, ENCODING)
        if (minBuffer <= 0) throw CaptureException.micUnavailable("48 kHz mono PCM16 recording is not supported")
        val record = try {
            AudioRecord.Builder()
                .setAudioSource(source)
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setSampleRate(AudioChunk.SAMPLE_RATE)
                        .setChannelMask(CHANNEL_MASK)
                        .setEncoding(ENCODING)
                        .build(),
                )
                // Room for 200 ms, so a late read never loses audio.
                .setBufferSizeInBytes(maxOf(minBuffer * 2, AudioChunk.SAMPLE_RATE * AudioChunk.BYTES_PER_SAMPLE / 5))
                .build()
        } catch (error: SecurityException) {
            throw CaptureException.permissionDenied("Microphone access was refused: ${error.message}")
        } catch (error: Exception) {
            throw CaptureException.micUnavailable("Cannot open the microphone: ${error.message}")
        }
        if (record.state != AudioRecord.STATE_INITIALIZED) {
            record.release()
            throw CaptureException.micUnavailable("The microphone failed to initialise")
        }
        return record
    }

    /** One recording: a blocking read loop on its own thread. */
    private inner class RecordRun(private val record: AudioRecord, private val listener: AudioSourceListener) {
        private val thread = Thread({ readLoop() }, "mwb-mic")
        private val callbackExecutor = Executors.newSingleThreadExecutor { task -> Thread(task, "mwb-mic-events") }

        @Volatile
        private var running = false
        private var silenced = false // callback executor

        private val recordingCallback = object : AudioManager.AudioRecordingCallback() {
            override fun onRecordingConfigChanged(configs: List<AudioRecordingConfiguration>) {
                val ours = configs.firstOrNull { it.clientAudioSessionId == record.audioSessionId } ?: return
                if (ours.isClientSilenced == silenced || !running) return
                silenced = ours.isClientSilenced
                // Another app (a call, an assistant) has priority: samples keep coming, as silence.
                listener.onAudioEvent(if (silenced) SourceEvent.Interrupted("silenced") else SourceEvent.Resumed)
            }
        }

        fun start() {
            record.registerAudioRecordingCallback(callbackExecutor, recordingCallback)
            record.startRecording()
            check(record.recordingState == AudioRecord.RECORDSTATE_RECORDING) { "the recording did not start" }
            running = true
            thread.start()
        }

        fun stop() {
            running = false
            runCatching { record.unregisterAudioRecordingCallback(recordingCallback) }
            runCatching { record.stop() } // unblocks the read
            if (thread.isAlive) thread.join(STOP_TIMEOUT_MS)
            record.release()
            callbackExecutor.shutdown()
        }

        private fun readLoop() {
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
            val buffer = ShortArray(FRAMES_PER_READ)
            val timestamp = AudioTimestamp()
            var framesRead = 0L
            var previous: BlockTime? = null
            try {
                while (running) {
                    val count = record.read(buffer, 0, buffer.size, AudioRecord.READ_BLOCKING)
                    if (!running) return
                    if (count < 0) {
                        listener.onAudioEvent(SourceEvent.Failed("Microphone read failed ($count)"))
                        return
                    }
                    if (count == 0) continue
                    val time = captureTime(timestamp, framesRead, count)
                    framesRead += count
                    // Compared with the previous block only (from the same time source), so slow
                    // clock drift never trips it: a jump means samples were lost (overrun).
                    val last = previous
                    val discontinuity = last == null ||
                        (last.precise == time.precise && abs(time.startUs - last.endUs) > DISCONTINUITY_TOLERANCE_US)
                    previous = time
                    listener.onAudioCaptured(Pcm16.toLittleEndian(buffer, count), time.startUs, discontinuity)
                }
            } catch (error: Exception) {
                // An exception escaping this thread would kill the app; report it instead.
                if (running) listener.onAudioEvent(SourceEvent.Failed("Microphone capture failed: ${error.message}"))
            }
        }

        /**
         * Capture time of the block just read: from the recording's timestamp (frame position at a
         * `CLOCK_BOOTTIME` instant) when available, else estimated from the read time.
         */
        private fun captureTime(timestamp: AudioTimestamp, framesBefore: Long, count: Int): BlockTime {
            val durationUs = count * 1_000_000L / AudioChunk.SAMPLE_RATE
            if (record.getTimestamp(timestamp, AudioTimestamp.TIMEBASE_BOOTTIME) == AudioRecord.SUCCESS) {
                val offsetFrames = framesBefore - timestamp.framePosition
                val startUs = (timestamp.nanoTime + offsetFrames * 1_000_000_000L / AudioChunk.SAMPLE_RATE) / 1_000
                return BlockTime(startUs, startUs + durationUs, precise = true)
            }
            val endUs = SystemClock.elapsedRealtimeNanos() / 1_000
            return BlockTime(endUs - durationUs, endUs, precise = false)
        }
    }

    private class BlockTime(val startUs: Long, val endUs: Long, val precise: Boolean)

    private companion object {
        const val CHANNEL_MASK = AudioFormat.CHANNEL_IN_MONO
        const val ENCODING = AudioFormat.ENCODING_PCM_16BIT

        /** 20 ms at 48 kHz. */
        const val FRAMES_PER_READ = 960
        const val DISCONTINUITY_TOLERANCE_US = 20_000L
        const val STOP_TIMEOUT_MS = 1_000L
    }
}

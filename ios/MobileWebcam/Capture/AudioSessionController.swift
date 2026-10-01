import AVFoundation
import BridgeKit

/// Configures the app audio session for microphone capture. The capture session is told not to
/// touch the audio session, so the mode chosen by the host (`StartAudio.processing`) sticks.
final class AudioSessionController: Sendable {
    private let logger: AppLogger

    init(logger: AppLogger) {
        self.logger = logger
    }

    func activate(processing: AudioProcessing) throws {
        let session = AVAudioSession.sharedInstance()
        let mode: AVAudioSession.Mode = switch processing {
        case .standard: .videoRecording
        case .raw: .measurement
        case .voice: .voiceChat
        }
        do {
            try session.setCategory(.record, mode: mode, options: [])
            // 48 kHz is native on current iPhones; asking for it avoids resampling.
            try? session.setPreferredSampleRate(Double(AudioChunk.sampleRate))
            try? session.setPreferredIOBufferDuration(0.01)
            try session.setActive(true)
        } catch {
            throw CaptureError.deviceUnavailable("Audio session activation failed: \(error.localizedDescription)")
        }
        logger.info("audio", "Audio session active: mode \(mode.rawValue), \(Int(session.sampleRate)) Hz")
    }

    func deactivate() {
        do {
            try AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
        } catch {
            logger.debug("audio", "Audio session deactivation failed: \(error.localizedDescription)")
        }
    }
}

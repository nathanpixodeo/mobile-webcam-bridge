import type { MediaError } from '#domain/errors.ts';
import type { EventSource } from '#domain/events.ts';

/** Fill state of the virtual microphone, reported by the driver through bridge-native. */
export interface AudioSinkStatus {
  readonly bufferedBytes: number;
  readonly capacityBytes: number;
  /** An app is currently capturing from the virtual microphone. */
  readonly streamActive: boolean;
  readonly underruns: number;
  readonly overruns: number;
}

export interface AudioSinkEvents {
  status: [status: AudioSinkStatus];
  /** The sink died (helper exited, driver removed); it cannot be used any more. */
  failed: [error: MediaError];
}

/** The virtual microphone. Accepts mono s16le PCM at 48 kHz. */
export interface AudioSink extends AsyncDisposable {
  readonly events: EventSource<AudioSinkEvents>;
  readonly sampleRate: number;
  /** Returns false if the sink is congested and dropped the oldest queued audio. */
  write(samples: Int16Array): boolean;
}

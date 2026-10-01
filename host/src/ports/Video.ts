import type { BridgeError, MediaError } from '#domain/errors.ts';
import type { EventSource } from '#domain/events.ts';
import type { PlaceholderKind } from '#domain/video/PlaceholderPolicy.ts';
import type { VideoMode } from '#domain/video/VideoMode.ts';

/** One H.264 access unit received from the device. */
export interface EncodedAccessUnit {
  readonly data: Buffer;
  readonly isIdr: boolean;
  readonly isDisposable: boolean;
  /** Capture time on the device clock (µs). */
  readonly timestampUs: bigint;
}

export interface VideoOutputEvents {
  /** Number of apps currently reading the virtual camera. */
  consumersChanged: [count: number];
  /** The output died (e.g. the hub process exited); it cannot be used any more. */
  failed: [error: BridgeError];
}

/** The virtual camera endpoint (bridge-native video hub). Decoders write raw NV12 to `ingestPath`. */
export interface VideoOutput extends AsyncDisposable {
  readonly ingestPath: string;
  readonly mode: VideoMode;
  readonly consumers: number;
  readonly events: EventSource<VideoOutputEvents>;
  setPlaceholder(kind: PlaceholderKind | null): void;
}

export interface VideoDecoderEvents {
  /** The decoder stopped unexpectedly; dispose it and create a new one. */
  failed: [error: MediaError];
}

export interface VideoDecoder extends AsyncDisposable {
  readonly events: EventSource<VideoDecoderEvents>;
  /**
   * Queues an access unit. Returns false when the decoder is congested and the access unit was
   * not accepted; the caller must then drop until the next IDR and request a keyframe.
   */
  decode(accessUnit: EncodedAccessUnit): boolean;
}

export interface VideoDecoderFactory {
  create(output: VideoOutput, signal: AbortSignal): Promise<VideoDecoder>;
}

import type { BridgeError, MediaError } from '#domain/errors.ts';
import type { EventSource } from '#domain/events.ts';
import type { PlaceholderKind } from '#domain/video/PlaceholderPolicy.ts';
import type { VideoMode, VideoSize } from '#domain/video/VideoMode.ts';

/** One H.264 access unit received from the device. */
export interface EncodedAccessUnit {
  readonly data: Buffer;
  readonly isIdr: boolean;
  readonly isDisposable: boolean;
  /** Capture time on the device clock (µs). */
  readonly timestampUs: bigint;
}

/** The apps currently reading the virtual camera. */
export interface ConsumerDemand {
  readonly count: number;
  /** The mode each consumer subscribed to, in subscription order (`modes.length === count`). */
  readonly modes: readonly VideoMode[];
}

export const NO_CONSUMERS: ConsumerDemand = { count: 0, modes: [] };

export interface VideoOutputEvents {
  consumersChanged: [demand: ConsumerDemand];
  /** The output died (e.g. the hub process exited); it cannot be used any more. */
  failed: [error: BridgeError];
}

/**
 * The virtual camera endpoint (bridge-native video hub). Decoders write raw NV12 frames of
 * `ingestMode` to `ingestPath`; the hub scales them to each consumer's mode.
 */
export interface VideoOutput extends AsyncDisposable {
  readonly ingestPath: string;
  /** The installed default camera mode, which is also the initial ingest size. */
  readonly defaultMode: VideoMode;
  /** Size of the frames the hub currently expects on the ingest pipe. */
  readonly ingestMode: VideoSize;
  readonly consumers: ConsumerDemand;
  readonly events: EventSource<VideoOutputEvents>;
  setPlaceholder(kind: PlaceholderKind | null): void;
  /**
   * Changes the ingest size; resolves once the hub has acknowledged it. The hub drops the current
   * ingest writer, so stop the decoder first. Rejects on timeout, abort or hub failure.
   */
  setIngestMode(size: VideoSize, signal: AbortSignal): Promise<void>;
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
  /** Starts a decoder writing frames of `mode`'s size into `output`'s ingest pipe. */
  create(output: VideoOutput, mode: VideoMode, signal: AbortSignal): Promise<VideoDecoder>;
}

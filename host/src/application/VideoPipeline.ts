import type { Clock } from '#domain/clock.ts';
import { toError, type MediaError } from '#domain/errors.ts';
import { KeyframeGate } from '#domain/h264/KeyframeGate.ts';
import { selectPlaceholder, type PlaceholderInputs } from '#domain/video/PlaceholderPolicy.ts';
import type { Logger } from '#ports/Logger.ts';
import type { Metrics } from '#ports/Metrics.ts';
import type { EncodedAccessUnit, VideoDecoder, VideoDecoderFactory, VideoOutput } from '#ports/Video.ts';

export interface VideoPipelineOptions {
  readonly output: VideoOutput;
  readonly decoders: VideoDecoderFactory;
  readonly clock: Clock;
  readonly logger: Logger;
  readonly metrics: Metrics;
  /** Asks the device for an IDR (wired to the active session). */
  readonly requestKeyframe: () => void;
  readonly keyframeRequestIntervalMs: number;
  readonly maxDecoderRestartsPerMinute: number;
}

/**
 * Access units → decoder → virtual camera. Owns the decoder lifecycle: started while the
 * camera has consumers, restarted (rate-limited) when it crashes, and gated so it only ever
 * starts decoding at an IDR.
 */
export class VideoPipeline implements AsyncDisposable {
  readonly #options: VideoPipelineOptions;
  readonly #gate = new KeyframeGate();
  readonly #abort = new AbortController();
  readonly #restartTimes: number[] = [];
  #decoder: VideoDecoder | undefined;
  #decoderSubscription: Disposable | undefined;
  #active = false;
  #lastKeyframeRequestMs = Number.NEGATIVE_INFINITY;
  #lastSeq: number | undefined;
  /** Serialises decoder start/stop so overlapping demand changes cannot race. */
  #lifecycle: Promise<void> = Promise.resolve();

  constructor(options: VideoPipelineOptions) {
    this.#options = options;
  }

  get isActive(): boolean {
    return this.#active;
  }

  /** Starts or stops decoding (driven by camera demand). */
  setActive(active: boolean): void {
    if (active === this.#active) return;
    this.#active = active;
    this.#enqueue(active ? () => this.#startDecoder() : () => this.#stopDecoder());
  }

  onAccessUnit(accessUnit: EncodedAccessUnit, seq: number): void {
    const { metrics } = this.#options;
    metrics.increment('video.accessUnits');
    metrics.increment('video.bytes', accessUnit.data.length);
    if (this.#lastSeq !== undefined && seq !== (this.#lastSeq + 1) >>> 0) {
      metrics.increment('video.deviceDrops', (seq - this.#lastSeq - 1) >>> 0);
    }
    this.#lastSeq = seq;

    const decoder = this.#decoder;
    if (decoder === undefined) return;
    if (!this.#gate.admit(accessUnit)) {
      metrics.increment('video.droppedAwaitingIdr');
      this.#requestKeyframe();
      return;
    }
    if (!decoder.decode(accessUnit)) {
      metrics.increment('video.droppedDecoderBusy');
      this.#gate.markLoss();
      this.#requestKeyframe();
    }
  }

  /** A new encoder configuration starts with an IDR; nothing to do but log it. */
  onConfigurationChanged(): void {
    this.#lastSeq = undefined;
  }

  /** Recomputes which placeholder (if any) the camera shows. */
  updatePlaceholder(inputs: PlaceholderInputs): void {
    this.#options.output.setPlaceholder(selectPlaceholder(inputs));
  }

  async [Symbol.asyncDispose](): Promise<void> {
    this.#abort.abort();
    this.#active = false;
    this.#enqueue(() => this.#stopDecoder());
    await this.#lifecycle;
  }

  #enqueue(operation: () => Promise<void>): void {
    this.#lifecycle = this.#lifecycle.then(operation).catch((error: unknown) => {
      this.#options.logger.error('Video pipeline operation failed', { error: toError(error).message });
    });
  }

  async #startDecoder(): Promise<void> {
    if (!this.#active || this.#decoder !== undefined || this.#abort.signal.aborted) return;
    const decoder = await this.#options.decoders.create(this.#options.output, this.#abort.signal);
    this.#decoder = decoder;
    this.#decoderSubscription = decoder.events.on('failed', (error) => {
      this.#onDecoderFailed(decoder, error);
    });
    this.#gate.markLoss();
    this.#lastKeyframeRequestMs = Number.NEGATIVE_INFINITY;
    this.#requestKeyframe();
    this.#options.logger.info('Video decoding started');
  }

  async #stopDecoder(): Promise<void> {
    const decoder = this.#decoder;
    if (decoder === undefined) return;
    this.#decoder = undefined;
    this.#decoderSubscription?.[Symbol.dispose]();
    this.#decoderSubscription = undefined;
    await decoder[Symbol.asyncDispose]();
    this.#options.logger.info('Video decoding stopped');
  }

  #onDecoderFailed(decoder: VideoDecoder, error: MediaError): void {
    if (decoder !== this.#decoder) return;
    const { clock, logger, maxDecoderRestartsPerMinute } = this.#options;
    this.#options.metrics.increment('video.decoderRestarts');
    const now = clock.nowMs();
    while (this.#restartTimes.length > 0 && now - (this.#restartTimes[0] ?? 0) > 60_000) this.#restartTimes.shift();
    this.#enqueue(() => this.#stopDecoder());
    if (this.#restartTimes.length >= maxDecoderRestartsPerMinute) {
      logger.error('Decoder keeps failing; video stays off until the camera is reopened', { error: error.message });
      this.#active = false;
      return;
    }
    this.#restartTimes.push(now);
    logger.warn('Decoder failed, restarting', { error: error.message });
    this.#enqueue(() => this.#startDecoder());
  }

  #requestKeyframe(): void {
    const now = this.#options.clock.nowMs();
    if (now - this.#lastKeyframeRequestMs < this.#options.keyframeRequestIntervalMs) return;
    this.#lastKeyframeRequestMs = now;
    this.#options.metrics.increment('video.keyframeRequests');
    this.#options.requestKeyframe();
  }
}

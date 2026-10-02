import type { Clock } from '#domain/clock.ts';
import { toError, type MediaError } from '#domain/errors.ts';
import { KeyframeGate } from '#domain/h264/KeyframeGate.ts';
import { isIngestSize } from '#domain/video/ModeCatalog.ts';
import { selectPlaceholder, type PlaceholderInputs } from '#domain/video/PlaceholderPolicy.ts';
import { describeMode, sameMode, sameSize, type VideoMode, type VideoSize } from '#domain/video/VideoMode.ts';
import type { Logger } from '#ports/Logger.ts';
import type { Metrics } from '#ports/Metrics.ts';
import type { EncodedAccessUnit, VideoDecoder, VideoDecoderFactory, VideoOutput } from '#ports/Video.ts';

export interface VideoPipelineOptions {
  readonly output: VideoOutput;
  readonly decoders: VideoDecoderFactory;
  /** Mode to decode to until `setMode` is called (the installed default mode). */
  readonly initialMode: VideoMode;
  readonly clock: Clock;
  readonly logger: Logger;
  readonly metrics: Metrics;
  /** Asks the device for an IDR (wired to the active session). */
  readonly requestKeyframe: () => void;
  readonly keyframeRequestIntervalMs: number;
  readonly maxDecoderRestartsPerMinute: number;
}

interface RunningDecoder {
  readonly decoder: VideoDecoder;
  readonly mode: VideoMode;
  readonly subscription: Disposable;
}

/**
 * Access units → decoder → virtual camera. Owns the decoder lifecycle: started while the
 * camera has consumers, restarted for every new decoded size, restarted (rate-limited) when it
 * crashes, and gated so it only ever starts decoding at an IDR.
 *
 * The decoder outputs the size the phone encodes (`VideoConfig`), so ffmpeg only resizes while a
 * switch is in flight: resizing inside ffmpeg holds back one more frame (measured with ffmpeg
 * 8.1), and the hub scales for every consumer anyway. Until the phone confirms, the requested
 * mode's size is assumed, which is what the phone normally sends.
 */
export class VideoPipeline implements AsyncDisposable {
  readonly #options: VideoPipelineOptions;
  readonly #gate = new KeyframeGate();
  readonly #abort = new AbortController();
  readonly #restartTimes: number[] = [];
  #running: RunningDecoder | undefined;
  #active = false;
  #mode: VideoMode;
  /** What the decoder outputs: the phone's encoded size, else the requested mode's size. */
  #decodeSize: VideoSize;
  #lastKeyframeRequestMs = Number.NEGATIVE_INFINITY;
  #lastSeq: number | undefined;
  /** Serialises decoder start/stop so overlapping demand and mode changes cannot race. */
  #lifecycle: Promise<void> = Promise.resolve();

  constructor(options: VideoPipelineOptions) {
    this.#options = options;
    this.#mode = options.initialMode;
    this.#decodeSize = sizeOf(options.initialMode);
  }

  get isActive(): boolean {
    return this.#active;
  }

  /** Starts or stops decoding (driven by camera demand). */
  setActive(active: boolean): void {
    if (active === this.#active) return;
    this.#active = active;
    this.#enqueue(() => this.#reconcile());
  }

  /** The streamed camera mode changed; the phone is asked for it and normally sends its size. */
  setMode(mode: VideoMode): void {
    if (sameMode(mode, this.#mode)) return;
    // A frame rate change keeps the encoded size the phone confirmed.
    if (!sameSize(mode, this.#mode)) this.#decodeSize = sizeOf(mode);
    this.#mode = mode;
    this.#enqueue(() => this.#reconcile());
  }

  onAccessUnit(accessUnit: EncodedAccessUnit, seq: number): void {
    const { metrics } = this.#options;
    metrics.increment('video.accessUnits');
    metrics.increment('video.bytes', accessUnit.data.length);
    if (this.#lastSeq !== undefined && seq !== (this.#lastSeq + 1) >>> 0) {
      metrics.increment('video.deviceDrops', (seq - this.#lastSeq - 1) >>> 0);
    }
    this.#lastSeq = seq;

    const decoder = this.#running?.decoder;
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

  /**
   * A new encoder configuration (it starts with an IDR). The decoder follows the encoded size; a
   * size the hub cannot ingest keeps the requested mode's size and lets ffmpeg letterbox.
   */
  onConfigurationChanged(encoded: VideoSize): void {
    this.#lastSeq = undefined;
    const size = isIngestSize(encoded) ? sizeOf(encoded) : sizeOf(this.#mode);
    if (sameSize(size, this.#decodeSize)) return;
    this.#decodeSize = size;
    this.#enqueue(() => this.#reconcile());
  }

  /** Recomputes which placeholder (if any) the camera shows. */
  updatePlaceholder(inputs: PlaceholderInputs): void {
    this.#options.output.setPlaceholder(selectPlaceholder(inputs));
  }

  async [Symbol.asyncDispose](): Promise<void> {
    this.#abort.abort();
    this.#active = false;
    this.#enqueue(() => this.#reconcile());
    await this.#lifecycle;
  }

  #enqueue(operation: () => Promise<void>): void {
    this.#lifecycle = this.#lifecycle.then(operation).catch((error: unknown) => {
      if (error === this.#abort.signal.reason) return;
      this.#options.logger.error('Video pipeline operation failed', { error: toError(error).message });
    });
  }

  /**
   * Brings the decoder in line with the demand and the mode. It reads the state when it runs, so
   * a burst of mode changes costs a single switch to the latest mode.
   */
  async #reconcile(): Promise<void> {
    const { output, decoders, logger } = this.#options;
    const signal = this.#abort.signal;
    if (!this.#wantsDecoder()) {
      await this.#stopDecoder();
      return;
    }
    if (this.#running !== undefined && sameSize(this.#running.mode, this.#decodeSize)) return;
    await this.#stopDecoder();
    // The hub must expect the new frame size before a decoder writes it; the size may move again
    // during the round trip, and only the latest one gets a decoder.
    let size = this.#decodeSize;
    while (this.#wantsDecoder() && !sameSize(output.ingestMode, size)) {
      await output.setIngestMode(size, signal);
      size = this.#decodeSize;
    }
    if (!this.#wantsDecoder()) return;
    const mode: VideoMode = { ...size, fpsNum: this.#mode.fpsNum, fpsDen: this.#mode.fpsDen };
    const decoder = await decoders.create(output, mode, signal);
    this.#running = {
      decoder,
      mode,
      subscription: decoder.events.on('failed', (error) => {
        this.#onDecoderFailed(decoder, error);
      }),
    };
    this.#gate.markLoss();
    this.#lastKeyframeRequestMs = Number.NEGATIVE_INFINITY;
    this.#requestKeyframe();
    logger.info('Video decoding started', { mode: describeMode(mode) });
  }

  #wantsDecoder(): boolean {
    return this.#active && !this.#abort.signal.aborted;
  }

  async #stopDecoder(): Promise<void> {
    const running = this.#running;
    if (running === undefined) return;
    this.#running = undefined;
    running.subscription[Symbol.dispose]();
    await running.decoder[Symbol.asyncDispose]();
    this.#options.logger.info('Video decoding stopped');
  }

  #onDecoderFailed(decoder: VideoDecoder, error: MediaError): void {
    if (decoder !== this.#running?.decoder) return;
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
    this.#enqueue(() => this.#reconcile());
  }

  #requestKeyframe(): void {
    const now = this.#options.clock.nowMs();
    if (now - this.#lastKeyframeRequestMs < this.#options.keyframeRequestIntervalMs) return;
    this.#lastKeyframeRequestMs = now;
    this.#options.metrics.increment('video.keyframeRequests');
    this.#options.requestKeyframe();
  }
}

function sizeOf(size: VideoSize): VideoSize {
  return { width: size.width, height: size.height };
}

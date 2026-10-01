import { PiController } from './PiController.ts';

export interface DriftCompensatorOptions {
  readonly sampleRate: number;
  readonly bytesPerFrame: number;
  /** Buffer fill (driver ring) the controller steers towards. */
  readonly targetFillMs: number;
  /** Proportional gain, ppm per millisecond of fill error. */
  readonly kp: number;
  /** Integral gain, ppm per (millisecond × second) of fill error. */
  readonly ki: number;
  /** Hard clamp of the resampling deviation. */
  readonly maxPpm: number;
  /** Maximum change of the deviation per second (keeps pitch changes inaudible). */
  readonly slewPpmPerSecond: number;
  /** Exponential smoothing factor for fill measurements, in (0, 1]. */
  readonly smoothing: number;
  /** Fill error beyond which the compensator drops audio instead of resampling. */
  readonly resyncThresholdMs: number;
}

export interface FillSample {
  readonly bufferedBytes: number;
  readonly streamActive: boolean;
  /** Monotonic underrun counter reported by the driver. */
  readonly underruns: number;
}

export type DriftCorrection =
  | { readonly kind: 'none' }
  | { readonly kind: 'insertSilence'; readonly frames: number }
  | { readonly kind: 'dropInput'; readonly frames: number };

export interface DriftUpdate {
  /** Output samples per input sample: > 1 stretches (fills the buffer), < 1 shrinks it. */
  readonly ratio: number;
  readonly correction: DriftCorrection;
}

const NO_CORRECTION: DriftCorrection = { kind: 'none' };

/**
 * Keeps the virtual microphone buffer near its target although the phone's audio clock and
 * the driver's clock run at slightly different rates (typically < 100 ppm). Small errors are
 * absorbed by resampling a few hundred ppm (inaudible); underruns and large overshoots are
 * corrected immediately with silence insertion or input dropping.
 */
export class AudioDriftCompensator {
  readonly #options: DriftCompensatorOptions;
  readonly #controller: PiController;
  #smoothedFillMs: number | undefined;
  #ppm = 0;
  #lastUpdateMs: number | undefined;
  #lastUnderruns: number | undefined;
  #wasActive = false;

  constructor(options: DriftCompensatorOptions) {
    if (!(options.smoothing > 0 && options.smoothing <= 1)) throw new RangeError('smoothing must be in (0, 1]');
    this.#options = options;
    this.#controller = new PiController({
      kp: options.kp,
      ki: options.ki,
      outputMin: -options.maxPpm,
      outputMax: options.maxPpm,
    });
  }

  get ratio(): number {
    return 1 + this.#ppm * 1e-6;
  }

  get deviationPpm(): number {
    return this.#ppm;
  }

  update(sample: FillSample, nowMs: number): DriftUpdate {
    const previousUnderruns = this.#lastUnderruns;
    this.#lastUnderruns = sample.underruns;

    if (!sample.streamActive) {
      this.reset();
      return { ratio: 1, correction: NO_CORRECTION };
    }
    const target = this.#options.targetFillMs;
    if (!this.#wasActive) {
      // Capture just started: prefill so the first reads do not underrun.
      this.reset();
      this.#wasActive = true;
      this.#lastUpdateMs = nowMs;
      return { ratio: 1, correction: { kind: 'insertSilence', frames: this.#msToFrames(target) } };
    }

    const fillMs = sample.bufferedBytes / this.#bytesPerMs();
    const dtSeconds = this.#lastUpdateMs === undefined ? 0 : Math.max(0, (nowMs - this.#lastUpdateMs) / 1000);
    this.#lastUpdateMs = nowMs;

    if (previousUnderruns !== undefined && sample.underruns > previousUnderruns) {
      this.#restartControl(target);
      return { ratio: this.ratio, correction: { kind: 'insertSilence', frames: this.#msToFrames(target - fillMs) } };
    }
    if (fillMs - target > this.#options.resyncThresholdMs) {
      this.#restartControl(target);
      return { ratio: this.ratio, correction: { kind: 'dropInput', frames: this.#msToFrames(fillMs - target) } };
    }

    const alpha = this.#options.smoothing;
    this.#smoothedFillMs =
      this.#smoothedFillMs === undefined ? fillMs : this.#smoothedFillMs + alpha * (fillMs - this.#smoothedFillMs);
    const wanted = this.#controller.update(target - this.#smoothedFillMs, dtSeconds);
    const maxStep = this.#options.slewPpmPerSecond * dtSeconds;
    this.#ppm += Math.max(-maxStep, Math.min(maxStep, wanted - this.#ppm));
    return { ratio: this.ratio, correction: NO_CORRECTION };
  }

  reset(): void {
    this.#controller.reset();
    this.#smoothedFillMs = undefined;
    this.#ppm = 0;
    this.#lastUpdateMs = undefined;
    this.#wasActive = false;
  }

  #restartControl(target: number): void {
    this.#controller.reset();
    this.#smoothedFillMs = target;
  }

  #bytesPerMs(): number {
    return (this.#options.sampleRate * this.#options.bytesPerFrame) / 1000;
  }

  #msToFrames(ms: number): number {
    return Math.max(0, Math.round((ms * this.#options.sampleRate) / 1000));
  }
}

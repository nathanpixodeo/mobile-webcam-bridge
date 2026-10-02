import type { Clock } from '#domain/clock.ts';
import { Emitter, type EventSource } from '#domain/events.ts';
import { maxFpsFor } from './ModeCatalog.ts';
import { compareModes, describeSize, framesPerSecond, modeArea, sameMode, type VideoMode } from './VideoMode.ts';

export interface ModeArbiterEvents {
  /** The phone should stream `mode` from now on. */
  changed: [mode: VideoMode];
}

export interface ModeArbiterOptions {
  /** Mode before any consumer subscribed (the installed default mode). */
  readonly initial: VideoMode;
  /** How long demand must stay below the current mode before a smaller mode is chosen. */
  readonly downgradeGraceMs: number;
  readonly clock: Clock;
}

/**
 * Chooses the mode the phone streams from the modes the camera consumers subscribed to: the
 * largest size and, separately, the highest frame rate, so the hub serves every consumer by
 * downscaling. Larger modes apply immediately; smaller ones only once no consumer has asked for
 * the current mode (or more) for `downgradeGraceMs`, because apps reopen the camera in other modes
 * while probing formats and each switch restarts the phone's encoder. Time without consumers
 * counts towards the grace, so a camera opened after a long pause gets its mode at once.
 */
export class ModeArbiter implements Disposable {
  readonly #options: ModeArbiterOptions;
  readonly #events = new Emitter<ModeArbiterEvents>();
  #mode: VideoMode;
  /** Since when no consumer has asked for the current mode or more; undefined while one does. */
  #belowSinceMs: number | undefined = Number.NEGATIVE_INFINITY;
  #downgradeTimer: Disposable | undefined;

  constructor(options: ModeArbiterOptions) {
    this.#options = options;
    this.#mode = options.initial;
  }

  get mode(): VideoMode {
    return this.#mode;
  }

  get events(): EventSource<ModeArbiterEvents> {
    return this.#events;
  }

  /** `modes` holds one entry per consumer; consumers subscribe to catalog modes only. */
  update(modes: readonly VideoMode[]): void {
    const { clock, downgradeGraceMs } = this.#options;
    this.#cancelDowngrade();
    if (modes.length === 0) {
      // Nothing to switch to: the last mode stays while the grace keeps running.
      this.#belowSinceMs ??= clock.nowMs();
      return;
    }
    const target = combinedMode(modes);
    if (sameMode(target, this.#mode)) {
      this.#belowSinceMs = undefined;
      return;
    }
    if (compareModes(target, this.#mode) > 0) {
      this.#set(target);
      return;
    }
    this.#belowSinceMs ??= clock.nowMs();
    const remainingMs = this.#belowSinceMs + downgradeGraceMs - clock.nowMs();
    if (remainingMs <= 0) {
      this.#set(target);
      return;
    }
    this.#downgradeTimer = clock.setTimeout(() => {
      this.#downgradeTimer = undefined;
      this.#set(target);
    }, remainingMs);
  }

  [Symbol.dispose](): void {
    this.#cancelDowngrade();
    this.#events.clear();
  }

  /** `mode` is always the combination of the current consumers, so they now ask for exactly it. */
  #set(mode: VideoMode): void {
    this.#mode = mode;
    this.#belowSinceMs = undefined;
    this.#events.emit('changed', mode);
  }

  #cancelDowngrade(): void {
    this.#downgradeTimer?.[Symbol.dispose]();
    this.#downgradeTimer = undefined;
  }
}

/**
 * Largest requested size at the highest requested frame rate, capped at the catalog's limit for
 * that size (1920x1080@30 + 1280x720@60 → 1920x1080@60, 3840x2160@30 + 1280x720@60 → 3840x2160@30).
 */
function combinedMode(modes: readonly VideoMode[]): VideoMode {
  const size = modes.reduce((largest, mode) => (modeArea(mode) > modeArea(largest) ? mode : largest));
  const maxFps = maxFpsFor(size);
  if (maxFps === undefined) throw new RangeError(`${describeSize(size)} is not a catalog size`);
  const fps = Math.max(...modes.map(framesPerSecond));
  return { width: size.width, height: size.height, fpsNum: Math.min(fps, maxFps), fpsDen: 1 };
}

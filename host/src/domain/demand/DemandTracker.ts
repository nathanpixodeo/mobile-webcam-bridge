import type { Clock } from '#domain/clock.ts';

export interface DemandTrackerOptions {
  /** How long demand must stay at zero before it is reported inactive. */
  readonly stopGraceMs: number;
  readonly clock: Clock;
  readonly onChange: (active: boolean) => void;
}

/**
 * Turns a raw consumer count into a debounced active/inactive signal: rising demand is reported
 * immediately, falling demand only after a grace period. Apps open and close the camera several
 * times while probing formats; the grace period keeps the phone from thrashing.
 */
export class DemandTracker implements Disposable {
  readonly #options: DemandTrackerOptions;
  #active = false;
  #stopTimer: Disposable | undefined;

  constructor(options: DemandTrackerOptions) {
    this.#options = options;
  }

  get active(): boolean {
    return this.#active;
  }

  update(consumers: number): void {
    if (consumers > 0) {
      this.#cancelStop();
      this.#set(true);
    } else if (this.#active && this.#stopTimer === undefined) {
      this.#stopTimer = this.#options.clock.setTimeout(() => {
        this.#stopTimer = undefined;
        this.#set(false);
      }, this.#options.stopGraceMs);
    }
  }

  [Symbol.dispose](): void {
    this.#cancelStop();
  }

  #set(active: boolean): void {
    if (active === this.#active) return;
    this.#active = active;
    this.#options.onChange(active);
  }

  #cancelStop(): void {
    this.#stopTimer?.[Symbol.dispose]();
    this.#stopTimer = undefined;
  }
}

export interface BackoffOptions {
  readonly initialMs: number;
  readonly maxMs: number;
  readonly factor: number;
  /** Relative jitter, e.g. 0.2 = ±20 %. */
  readonly jitter: number;
  /** Injected for deterministic tests; returns a number in [0, 1). */
  readonly random?: () => number;
}

/** Exponential backoff with symmetric jitter and a cap. */
export class ExponentialBackoff {
  readonly #options: Required<BackoffOptions>;
  #attempt = 0;

  constructor(options: BackoffOptions) {
    if (options.initialMs <= 0 || options.maxMs < options.initialMs || options.factor < 1) {
      throw new RangeError('Invalid backoff options');
    }
    this.#options = { random: Math.random, ...options };
  }

  get attempt(): number {
    return this.#attempt;
  }

  next(): number {
    const { initialMs, maxMs, factor, jitter, random } = this.#options;
    const base = Math.min(maxMs, initialMs * factor ** this.#attempt);
    this.#attempt++;
    const spread = base * jitter * (random() * 2 - 1);
    return Math.max(0, Math.round(Math.min(maxMs, base + spread)));
  }

  reset(): void {
    this.#attempt = 0;
  }
}

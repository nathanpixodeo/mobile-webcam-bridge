export interface TimingSample {
  /** Local clock when the Ping was sent. */
  readonly t0: bigint;
  /** Peer clock when the Ping arrived. */
  readonly t1: bigint;
  /** Peer clock when the Pong was sent. */
  readonly t2: bigint;
  /** Local clock when the Pong arrived. */
  readonly t3: bigint;
}

interface Estimate {
  readonly rttUs: bigint;
  readonly offsetUs: bigint;
}

/**
 * NTP-style clock offset estimation (protocol/SPEC.md §3.1). Keeps the minimum-RTT sample of
 * the last `window` exchanges: the sample least distorted by queuing gives the best offset.
 */
export class ClockOffsetEstimator {
  readonly #window: number;
  readonly #samples: Estimate[] = [];

  constructor(window = 16) {
    this.#window = window;
  }

  add(sample: TimingSample): void {
    const rttUs = sample.t3 - sample.t0 - (sample.t2 - sample.t1);
    if (rttUs < 0n) return; // clock went backwards or corrupt sample
    const offsetUs = (sample.t1 - sample.t0 + (sample.t2 - sample.t3)) / 2n;
    this.#samples.push({ rttUs, offsetUs });
    if (this.#samples.length > this.#window) this.#samples.shift();
  }

  #best(): Estimate | undefined {
    let best: Estimate | undefined;
    for (const sample of this.#samples) {
      if (best === undefined || sample.rttUs < best.rttUs) best = sample;
    }
    return best;
  }

  /** Peer clock minus local clock, or undefined before the first sample. */
  get offsetUs(): bigint | undefined {
    return this.#best()?.offsetUs;
  }

  get rttUs(): bigint | undefined {
    return this.#best()?.rttUs;
  }

  /** Converts a peer timestamp into the local clock. */
  toLocal(peerUs: bigint): bigint | undefined {
    const offset = this.offsetUs;
    return offset === undefined ? undefined : peerUs - offset;
  }

  reset(): void {
    this.#samples.length = 0;
  }
}

/**
 * Time abstraction. Domain and application code never touch `Date`, `performance` or global
 * timers directly, so tests can drive time deterministically with a fake clock.
 */
export interface Clock {
  /** Monotonic milliseconds. */
  nowMs(): number;
  /** Monotonic microseconds (same time base as the wire protocol `timestampUs`). */
  nowUs(): bigint;
  /** Resolves after `ms`; rejects with the abort reason if `signal` aborts first. */
  sleep(ms: number, signal?: AbortSignal): Promise<void>;
  /** One-shot timer; dispose to cancel. */
  setTimeout(callback: () => void, ms: number): Disposable;
  /** Repeating timer; dispose to cancel. */
  setInterval(callback: () => void, ms: number): Disposable;
}

/** Races `promise` against a timeout; rejects with `onTimeout()` when the timer fires first. */
export async function withTimeout<T>(
  clock: Clock,
  promise: Promise<T>,
  ms: number,
  onTimeout: () => Error,
): Promise<T> {
  let timer: Disposable | undefined;
  const timeout = new Promise<never>((_, reject) => {
    timer = clock.setTimeout(() => {
      reject(onTimeout());
    }, ms);
  });
  try {
    return await Promise.race([promise, timeout]);
  } finally {
    timer?.[Symbol.dispose]();
  }
}

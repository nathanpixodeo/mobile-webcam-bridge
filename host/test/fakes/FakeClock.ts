import { abortReason } from '#domain/abort.ts';
import type { Clock } from '#domain/clock.ts';

interface Timer {
  readonly id: number;
  dueMs: number;
  readonly intervalMs: number | undefined;
  readonly callback: () => void;
}

/**
 * Deterministic clock for tests. Time only moves through `advance()`, which fires due timers in
 * order and lets pending promise callbacks run between them.
 */
export class FakeClock implements Clock {
  #nowMs: number;
  #nextId = 1;
  readonly #timers = new Map<number, Timer>();

  constructor(startMs = 1_000_000) {
    this.#nowMs = startMs;
  }

  nowMs(): number {
    return this.#nowMs;
  }

  nowUs(): bigint {
    return BigInt(Math.round(this.#nowMs * 1000));
  }

  sleep(ms: number, signal?: AbortSignal): Promise<void> {
    return new Promise((resolve, reject) => {
      if (signal?.aborted === true) {
        reject(abortReason(signal));
        return;
      }
      const timer = this.setTimeout(() => {
        signal?.removeEventListener('abort', onAbort);
        resolve();
      }, ms);
      const onAbort = (): void => {
        timer[Symbol.dispose]();
        reject(abortReason(signal));
      };
      signal?.addEventListener('abort', onAbort, { once: true });
    });
  }

  setTimeout(callback: () => void, ms: number): Disposable {
    return this.#add(callback, ms, undefined);
  }

  setInterval(callback: () => void, ms: number): Disposable {
    return this.#add(callback, ms, Math.max(1, ms));
  }

  get pendingTimers(): number {
    return this.#timers.size;
  }

  /** Advances time by `ms`, firing every timer that becomes due, in due order. */
  async advance(ms: number): Promise<void> {
    const target = this.#nowMs + ms;
    await flushMicrotasks();
    for (;;) {
      const next = this.#nextDue();
      if (next === undefined || next.dueMs > target) break;
      this.#nowMs = next.dueMs;
      if (next.intervalMs === undefined) this.#timers.delete(next.id);
      else next.dueMs += next.intervalMs;
      next.callback();
      await flushMicrotasks();
    }
    this.#nowMs = target;
    await flushMicrotasks();
  }

  #add(callback: () => void, ms: number, intervalMs: number | undefined): Disposable {
    const id = this.#nextId++;
    this.#timers.set(id, { id, dueMs: this.#nowMs + Math.max(0, ms), intervalMs, callback });
    return {
      [Symbol.dispose]: () => {
        this.#timers.delete(id);
      },
    };
  }

  #nextDue(): Timer | undefined {
    let best: Timer | undefined;
    for (const timer of this.#timers.values()) {
      if (best === undefined || timer.dueMs < best.dueMs || (timer.dueMs === best.dueMs && timer.id < best.id)) {
        best = timer;
      }
    }
    return best;
  }
}

/** Lets queued promise reactions run (several hops, for chained awaits). */
export async function flushMicrotasks(rounds = 10): Promise<void> {
  for (let i = 0; i < rounds; i++) await Promise.resolve();
  await new Promise<void>((resolve) => setImmediate(resolve));
}

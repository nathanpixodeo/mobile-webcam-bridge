import type { MetricsRegistry, MetricsSnapshot } from '#ports/Metrics.ts';

const MAX_SAMPLES = 4096;

/** In-process metrics: counters and distributions reset on every `drain()`, gauges persist. */
export class InMemoryMetrics implements MetricsRegistry {
  readonly #counters = new Map<string, number>();
  readonly #gauges = new Map<string, number>();
  readonly #samples = new Map<string, number[]>();

  increment(name: string, by = 1): void {
    this.#counters.set(name, (this.#counters.get(name) ?? 0) + by);
  }

  gauge(name: string, value: number): void {
    this.#gauges.set(name, value);
  }

  observe(name: string, value: number): void {
    let samples = this.#samples.get(name);
    if (samples === undefined) {
      samples = [];
      this.#samples.set(name, samples);
    }
    if (samples.length < MAX_SAMPLES) samples.push(value);
  }

  drain(): MetricsSnapshot {
    const distributions: Record<string, { count: number; p50: number; p95: number; max: number }> = {};
    for (const [name, samples] of this.#samples) {
      if (samples.length === 0) continue;
      const sorted = [...samples].sort((a, b) => a - b);
      distributions[name] = {
        count: sorted.length,
        p50: percentile(sorted, 0.5),
        p95: percentile(sorted, 0.95),
        max: sorted.at(-1) ?? 0,
      };
    }
    const snapshot: MetricsSnapshot = {
      counters: Object.fromEntries(this.#counters),
      gauges: Object.fromEntries(this.#gauges),
      distributions,
    };
    this.#counters.clear();
    this.#samples.clear();
    return snapshot;
  }
}

function percentile(sorted: readonly number[], q: number): number {
  const index = Math.min(sorted.length - 1, Math.max(0, Math.ceil(q * sorted.length) - 1));
  return sorted[index] ?? 0;
}

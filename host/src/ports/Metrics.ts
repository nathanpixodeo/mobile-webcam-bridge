/** Minimal metrics port: monotonic counters, last-value gauges and sampled distributions. */
export interface Metrics {
  increment(name: string, by?: number): void;
  gauge(name: string, value: number): void;
  observe(name: string, value: number): void;
}

export interface MetricsSnapshot {
  readonly counters: Readonly<Record<string, number>>;
  readonly gauges: Readonly<Record<string, number>>;
  readonly distributions: Readonly<
    Record<string, { readonly count: number; readonly p50: number; readonly p95: number; readonly max: number }>
  >;
}

/** Metrics that can be read and reset periodically by a reporter. */
export interface MetricsRegistry extends Metrics {
  /** Returns the values gathered since the previous call and resets counters/distributions. */
  drain(): MetricsSnapshot;
}

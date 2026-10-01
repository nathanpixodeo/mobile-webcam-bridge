import type { Clock } from '#domain/clock.ts';
import type { Logger } from '#ports/Logger.ts';
import type { MetricsRegistry } from '#ports/Metrics.ts';
import type { DeviceManager } from './DeviceManager.ts';

export interface MetricsReporterOptions {
  readonly clock: Clock;
  readonly logger: Logger;
  readonly metrics: MetricsRegistry;
  readonly manager: DeviceManager;
  readonly metricsIntervalMs: number;
}

/** Logs one compact line of stream health every interval while media is flowing. */
export class MetricsReporter implements Disposable {
  readonly #timer: Disposable;

  constructor(options: MetricsReporterOptions) {
    const { clock, logger, metrics, manager, metricsIntervalMs } = options;
    let last = clock.nowMs();
    this.#timer = clock.setInterval(() => {
      const now = clock.nowMs();
      const seconds = Math.max(0.001, (now - last) / 1000);
      last = now;
      const snapshot = metrics.drain();
      const counter = (name: string): number => snapshot.counters[name] ?? 0;
      const accessUnits = counter('video.accessUnits');
      const audioChunks = counter('audio.chunks');
      if (accessUnits === 0 && audioChunks === 0) return;
      const timing = manager.activeSession?.timing;
      logger.info('Stream health', {
        fps: round(accessUnits / seconds),
        kbps: round((counter('video.bytes') * 8) / 1000 / seconds),
        deviceDrops: counter('video.deviceDrops'),
        hostDrops: counter('video.droppedDecoderBusy') + counter('video.droppedAwaitingIdr'),
        keyframeRequests: counter('video.keyframeRequests'),
        decoderRestarts: counter('video.decoderRestarts'),
        audioFillMs: round(snapshot.gauges['audio.fillMs'] ?? 0),
        audioDriftPpm: snapshot.gauges['audio.driftPpm'] ?? 0,
        audioGaps: counter('audio.deviceGaps'),
        audioSilenceFrames: counter('audio.silenceFrames'),
        rttMs: timing?.rttUs === undefined ? undefined : round(Number(timing.rttUs) / 1000),
      });
    }, metricsIntervalMs);
  }

  [Symbol.dispose](): void {
    this.#timer[Symbol.dispose]();
  }
}

function round(value: number): number {
  return Math.round(value * 10) / 10;
}

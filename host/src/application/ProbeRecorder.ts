import type { Clock } from '#domain/clock.ts';
import { TransportError } from '#domain/errors.ts';
import { nextEvent } from '#domain/events.ts';
import type { StartAudio, StartVideo } from '#domain/protocol/messages.ts';
import type { DeviceWatcher, MobileDevice } from '#ports/DeviceWatcher.ts';
import type { Logger } from '#ports/Logger.ts';
import type { DeviceSession } from './DeviceSession.ts';

/** Where the probe writes what it receives. */
export interface ProbeSinks {
  writeVideo(accessUnit: Buffer): void;
  writeAudio(samples: Int16Array): void;
  writeRecord(record: Readonly<Record<string, unknown>>): void;
}

export interface ProbeOptions {
  readonly seconds: number;
  readonly video: StartVideo | null;
  readonly audio: StartAudio | null;
  readonly waitForDeviceMs: number;
  readonly signal: AbortSignal;
}

export interface ProbeSummary {
  readonly device: string;
  readonly app: string | undefined;
  readonly seconds: number;
  readonly accessUnits: number;
  readonly keyframes: number;
  readonly fps: number;
  readonly kbps: number;
  readonly meanKeyframeIntervalMs: number | undefined;
  readonly videoSeqGaps: number;
  readonly audioSamples: number;
  /** Device audio clock rate relative to the host clock, in ppm (positive = device faster). */
  readonly audioRatePpm: number | undefined;
  readonly audioSeqGaps: number;
  readonly rttMs: number | undefined;
}

/**
 * Records the raw device streams for a fixed duration (diagnostics and test fixtures): every
 * access unit, all PCM, and a JSON line per protocol event.
 */
export class ProbeRecorder {
  readonly #watcher: DeviceWatcher;
  readonly #createSession: (device: MobileDevice) => DeviceSession;
  readonly #clock: Clock;
  readonly #logger: Logger;

  constructor(options: {
    watcher: DeviceWatcher;
    createSession: (device: MobileDevice) => DeviceSession;
    clock: Clock;
    logger: Logger;
  }) {
    this.#watcher = options.watcher;
    this.#createSession = options.createSession;
    this.#clock = options.clock;
    this.#logger = options.logger;
  }

  async record(options: ProbeOptions, sinks: ProbeSinks): Promise<ProbeSummary> {
    const device = await this.#waitForDevice(options);
    await using session = this.#createSession(device);
    const stats = new ProbeStats(this.#clock);
    using _subscriptions = this.#subscribe(session, sinks, stats);
    session.setDesiredStreams({ video: options.video, audio: options.audio });
    session.start();

    const ready = await this.#waitForReady(session, options);
    this.#logger.info('Recording', { seconds: options.seconds });
    stats.start();
    await this.#clock.sleep(options.seconds * 1000, options.signal).catch(() => undefined);
    return stats.summary(device.id, `${ready.app.name} ${ready.app.version}`, session.timing.rttUs);
  }

  async #waitForDevice(options: ProbeOptions): Promise<MobileDevice> {
    await this.#watcher.start();
    const present = this.#watcher.devices()[0];
    if (present !== undefined) return present;
    this.#logger.info('Waiting for a phone', { timeoutMs: options.waitForDeviceMs });
    const timeout = AbortSignal.any([options.signal, AbortSignal.timeout(options.waitForDeviceMs)]);
    try {
      const [device] = await nextEvent(this.#watcher.events, 'attached', timeout);
      return device;
    } catch {
      throw new TransportError('DEVICE_UNAVAILABLE', 'No phone connected');
    }
  }

  async #waitForReady(
    session: DeviceSession,
    options: ProbeOptions,
  ): Promise<{ app: { name: string; version: string } }> {
    for (;;) {
      const state = session.state;
      if (state.kind === 'ready') return state.peer;
      if (state.kind === 'stalled') throw state.reason;
      const [next] = await nextEvent(session.events, 'state', options.signal);
      if (next.kind === 'ready') return next.peer;
      if (next.kind === 'stalled') throw next.reason;
    }
  }

  #subscribe(session: DeviceSession, sinks: ProbeSinks, stats: ProbeStats): DisposableStack {
    const stack = new DisposableStack();
    stack.use(
      session.events.on('accessUnit', (accessUnit, seq) => {
        sinks.writeVideo(accessUnit.data);
        stats.onAccessUnit(accessUnit.data.length, accessUnit.isIdr, seq);
        sinks.writeRecord({
          type: 'VideoAccessUnit',
          seq,
          bytes: accessUnit.data.length,
          idr: accessUnit.isIdr,
          tUs: accessUnit.timestampUs.toString(),
        });
      }),
    );
    stack.use(
      session.events.on('audioChunk', (chunk) => {
        sinks.writeAudio(chunk.samples);
        stats.onAudio(chunk.samples.length, chunk.seq);
        sinks.writeRecord({
          type: 'AudioChunk',
          seq: chunk.seq,
          samples: chunk.samples.length,
          discontinuity: chunk.discontinuity,
          tUs: chunk.timestampUs.toString(),
        });
      }),
    );
    stack.use(
      session.events.on('videoConfig', (config) => {
        sinks.writeRecord({ type: 'VideoConfig', ...config });
      }),
    );
    stack.use(
      session.events.on('audioConfig', (config) => {
        sinks.writeRecord({ type: 'AudioConfig', ...config });
      }),
    );
    stack.use(
      session.events.on('status', (status) => {
        sinks.writeRecord({ type: 'Status', ...status });
      }),
    );
    stack.use(
      session.events.on('remoteLog', (entry) => {
        sinks.writeRecord({ type: 'Log', ...entry });
        this.#logger.info(`[ios] ${entry.message}`, { level: entry.level, category: entry.category });
      }),
    );
    stack.use(
      session.events.on('state', (state) => {
        sinks.writeRecord({ type: 'SessionState', state: state.kind });
      }),
    );
    return stack;
  }
}

class ProbeStats {
  readonly #clock: Clock;
  #startMs = 0;
  #recording = false;
  #accessUnits = 0;
  #bytes = 0;
  #keyframeTimes: number[] = [];
  #videoSeq: number | undefined;
  #videoGaps = 0;
  #audioSamples = 0;
  #audioSeq: number | undefined;
  #audioGaps = 0;

  constructor(clock: Clock) {
    this.#clock = clock;
  }

  start(): void {
    this.#startMs = this.#clock.nowMs();
    this.#recording = true;
  }

  onAccessUnit(bytes: number, isIdr: boolean, seq: number): void {
    if (this.#videoSeq !== undefined && seq !== (this.#videoSeq + 1) >>> 0) this.#videoGaps++;
    this.#videoSeq = seq;
    if (!this.#recording) return;
    this.#accessUnits++;
    this.#bytes += bytes;
    if (isIdr) this.#keyframeTimes.push(this.#clock.nowMs());
  }

  onAudio(samples: number, seq: number): void {
    if (this.#audioSeq !== undefined && seq !== (this.#audioSeq + 1) >>> 0) this.#audioGaps++;
    this.#audioSeq = seq;
    if (this.#recording) this.#audioSamples += samples;
  }

  summary(device: string, app: string | undefined, rttUs: bigint | undefined): ProbeSummary {
    const seconds = Math.max(0.001, (this.#clock.nowMs() - this.#startMs) / 1000);
    const intervals = this.#keyframeTimes.slice(1).map((time, i) => time - (this.#keyframeTimes[i] ?? time));
    return {
      device,
      app,
      seconds,
      accessUnits: this.#accessUnits,
      keyframes: this.#keyframeTimes.length,
      fps: this.#accessUnits / seconds,
      kbps: (this.#bytes * 8) / 1000 / seconds,
      meanKeyframeIntervalMs:
        intervals.length === 0 ? undefined : intervals.reduce((a, b) => a + b, 0) / intervals.length,
      videoSeqGaps: this.#videoGaps,
      audioSamples: this.#audioSamples,
      audioRatePpm: this.#audioSamples === 0 ? undefined : (this.#audioSamples / seconds / 48_000 - 1) * 1e6,
      audioSeqGaps: this.#audioGaps,
      rttMs: rttUs === undefined ? undefined : Number(rttUs) / 1000,
    };
  }
}

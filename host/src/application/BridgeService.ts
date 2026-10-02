import type { Clock } from '#domain/clock.ts';
import { DemandTracker } from '#domain/demand/DemandTracker.ts';
import type { DeviceStatus, StartAudio, StartVideo } from '#domain/protocol/messages.ts';
import { ModeArbiter } from '#domain/video/ModeArbiter.ts';
import { describeMode, type VideoMode } from '#domain/video/VideoMode.ts';
import type { AudioSink } from '#ports/Audio.ts';
import type { Logger } from '#ports/Logger.ts';
import type { MetricsRegistry } from '#ports/Metrics.ts';
import type { ConsumerDemand, VideoOutput } from '#ports/Video.ts';
import type { AudioPipeline } from './AudioPipeline.ts';
import type { DeviceManager } from './DeviceManager.ts';
import type { DeviceSession } from './DeviceSession.ts';
import { MetricsReporter } from './MetricsReporter.ts';
import { StreamCoordinator } from './StreamCoordinator.ts';
import type { VideoPipeline } from './VideoPipeline.ts';

export interface BridgeServiceDependencies {
  readonly manager: DeviceManager;
  readonly videoOutput: VideoOutput;
  readonly videoPipeline: VideoPipeline;
  readonly audio: { readonly sink: AudioSink; readonly pipeline: AudioPipeline } | undefined;
  readonly streams: { readonly video: (mode: VideoMode) => StartVideo; readonly audio: StartAudio };
  readonly videoStopGraceMs: number;
  readonly modeDowngradeGraceMs: number;
  readonly audioStopGraceMs: number;
  readonly metricsIntervalMs: number;
  readonly clock: Clock;
  readonly logger: Logger;
  readonly metrics: MetricsRegistry;
  /** Infrastructure owned by the service (hub, mic feeder, watcher …), disposed last. */
  readonly resources: AsyncDisposableStack;
}

/**
 * The running bridge. Connects the demand signals (camera consumers and their modes, microphone
 * capture) to the active device session, and the session's media to the video and audio pipelines.
 */
export class BridgeService implements AsyncDisposable {
  readonly #deps: BridgeServiceDependencies;
  readonly #subscriptions = new DisposableStack();
  readonly #coordinator: StreamCoordinator;
  readonly #videoDemand: DemandTracker;
  readonly #audioDemand: DemandTracker;
  readonly #modeArbiter: ModeArbiter;
  #sessionSubscriptions: DisposableStack | undefined;
  #lastStatus: DeviceStatus | undefined;

  constructor(deps: BridgeServiceDependencies) {
    this.#deps = deps;
    this.#modeArbiter = new ModeArbiter({
      initial: deps.videoOutput.defaultMode,
      downgradeGraceMs: deps.modeDowngradeGraceMs,
      clock: deps.clock,
    });
    this.#coordinator = new StreamCoordinator({
      video: deps.streams.video(this.#modeArbiter.mode),
      audio: deps.audio === undefined ? null : deps.streams.audio,
      onChange: (desired) => {
        deps.manager.activeSession?.setDesiredStreams(desired);
      },
    });
    this.#videoDemand = new DemandTracker({
      stopGraceMs: deps.videoStopGraceMs,
      clock: deps.clock,
      onChange: (active) => {
        deps.logger.info(active ? 'Camera opened by an app' : 'Camera no longer in use');
        this.#coordinator.setVideoDemand(active);
        deps.videoPipeline.setActive(active);
      },
    });
    this.#audioDemand = new DemandTracker({
      stopGraceMs: deps.audioStopGraceMs,
      clock: deps.clock,
      onChange: (active) => {
        deps.logger.info(active ? 'Microphone opened by an app' : 'Microphone no longer in use');
        this.#coordinator.setAudioDemand(active);
      },
    });
  }

  /** Runs until `signal` aborts or an essential component fails (then rejects). */
  async run(signal: AbortSignal): Promise<void> {
    const { manager, videoOutput, audio, logger } = this.#deps;
    const fatal = new Promise<never>((_, reject) => {
      this.#subscriptions.use(
        videoOutput.events.on('failed', (error) => {
          reject(error);
        }),
      );
    });
    this.#subscriptions.use(
      this.#modeArbiter.events.on('changed', (mode) => {
        this.#onModeChanged(mode);
      }),
    );
    this.#subscriptions.use(
      videoOutput.events.on('consumersChanged', (demand) => {
        this.#onConsumers(demand);
      }),
    );
    if (audio !== undefined) {
      this.#subscriptions.use(
        audio.sink.events.on('status', (status) => {
          audio.pipeline.onSinkStatus(status);
          this.#audioDemand.update(status.streamActive ? 1 : 0);
        }),
      );
      this.#subscriptions.use(
        audio.sink.events.on('failed', (error) => {
          logger.error('Virtual microphone stopped working', { error: error.message });
          this.#audioDemand.update(0);
        }),
      );
    }
    this.#subscriptions.use(
      manager.events.on('sessionStarted', (session) => {
        this.#attachSession(session);
      }),
    );
    this.#subscriptions.use(
      manager.events.on('sessionEnded', () => {
        this.#sessionSubscriptions?.dispose();
        this.#sessionSubscriptions = undefined;
        this.#lastStatus = undefined;
        this.#refreshPlaceholder();
      }),
    );
    this.#subscriptions.use(new MetricsReporter(this.#deps));

    this.#refreshPlaceholder();
    await manager.start();
    this.#onConsumers(videoOutput.consumers);
    logger.info('Bridge running');

    const aborted = new Promise<void>((resolve) => {
      if (signal.aborted) resolve();
      signal.addEventListener(
        'abort',
        () => {
          resolve();
        },
        { once: true },
      );
    });
    await Promise.race([aborted, fatal]);
  }

  async [Symbol.asyncDispose](): Promise<void> {
    this.#subscriptions.dispose();
    this.#modeArbiter[Symbol.dispose]();
    this.#videoDemand[Symbol.dispose]();
    this.#audioDemand[Symbol.dispose]();
    await this.#deps.manager[Symbol.asyncDispose]();
    this.#sessionSubscriptions?.dispose();
    await this.#deps.videoPipeline[Symbol.asyncDispose]();
    await this.#deps.resources.disposeAsync();
  }

  #onConsumers(demand: ConsumerDemand): void {
    // Mode first: a stream started by the demand then starts directly in the mode it needs,
    // instead of starting in the previous mode and switching right away.
    this.#modeArbiter.update(demand.modes);
    this.#videoDemand.update(demand.count);
  }

  #onModeChanged(mode: VideoMode): void {
    this.#deps.logger.info('Camera mode', { mode: describeMode(mode) });
    this.#coordinator.setVideoParams(this.#deps.streams.video(mode));
    this.#deps.videoPipeline.setMode(mode);
  }

  #attachSession(session: DeviceSession): void {
    const { videoPipeline, audio, logger } = this.#deps;
    this.#sessionSubscriptions?.dispose();
    const subscriptions = new DisposableStack();
    this.#sessionSubscriptions = subscriptions;
    const phoneLogger = logger.child({
      component: session.device.platform === 'unknown' ? 'phone' : session.device.platform,
      device: session.device.id,
    });

    subscriptions.use(
      session.events.on('state', (state) => {
        if (state.kind === 'ready') audio?.pipeline.reset();
        if (state.kind !== 'ready') this.#lastStatus = undefined;
        this.#refreshPlaceholder();
      }),
    );
    subscriptions.use(
      session.events.on('videoConfig', (config) => {
        logger.info('Video configuration', {
          size: `${config.width}x${config.height}`,
          fps: config.fps,
          kbps: config.bitrateKbps,
          encoder: config.encoder,
          camera: config.camera,
        });
        videoPipeline.onConfigurationChanged(config);
      }),
    );
    subscriptions.use(
      session.events.on('accessUnit', (accessUnit, seq) => {
        videoPipeline.onAccessUnit(accessUnit, seq);
      }),
    );
    if (audio !== undefined) {
      subscriptions.use(
        session.events.on('audioConfig', () => {
          audio.pipeline.reset();
        }),
      );
      subscriptions.use(
        session.events.on('audioChunk', (chunk) => {
          audio.pipeline.onAudioChunk(chunk);
        }),
      );
    }
    subscriptions.use(
      session.events.on('status', (status) => {
        this.#logStatusChange(status);
        this.#lastStatus = status;
        this.#refreshPlaceholder();
      }),
    );
    subscriptions.use(
      session.events.on('remoteLog', (entry, hostTimeUs) => {
        phoneLogger[entry.level](entry.message, { category: entry.category, hostTimeUs: hostTimeUs?.toString() });
      }),
    );
    session.setDesiredStreams(this.#coordinator.desired);
    this.#refreshPlaceholder();
  }

  #logStatusChange(status: DeviceStatus): void {
    const previous = this.#lastStatus;
    if (previous?.video.state !== status.video.state || previous.video.reason !== status.video.reason) {
      this.#deps.logger.info('Phone video', { state: status.video.state, reason: status.video.reason });
    }
    if (previous?.thermal !== status.thermal && status.thermal !== 'nominal') {
      this.#deps.logger.warn('Phone thermal state', { thermal: status.thermal });
    }
    for (const kind of ['camera', 'microphone'] as const) {
      if (status.permissions[kind] === 'denied' && previous?.permissions[kind] !== 'denied') {
        this.#deps.logger.warn(
          `Phone ${kind} permission denied. Allow it for the Mobile Webcam app in the phone's settings.`,
        );
      }
    }
  }

  #refreshPlaceholder(): void {
    const session = this.#deps.manager.activeSession;
    this.#deps.videoPipeline.updatePlaceholder({
      deviceAttached: session !== undefined,
      sessionReady: session?.state.kind === 'ready',
      videoState: this.#lastStatus?.video.state,
      videoReason: this.#lastStatus?.video.reason,
    });
  }
}

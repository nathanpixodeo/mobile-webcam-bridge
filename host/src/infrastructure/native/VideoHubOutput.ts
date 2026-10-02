import { randomBytes } from 'node:crypto';
import { abortReason } from '#domain/abort.ts';
import { MediaError } from '#domain/errors.ts';
import { Emitter, type EventSource } from '#domain/events.ts';
import type { ModeCap } from '#domain/video/ModeCatalog.ts';
import type { PlaceholderKind } from '#domain/video/PlaceholderPolicy.ts';
import {
  describeMode,
  describeSize,
  sameMode,
  sameSize,
  type VideoMode,
  type VideoSize,
} from '#domain/video/VideoMode.ts';
import type { Logger } from '#ports/Logger.ts';
import { NO_CONSUMERS, type ConsumerDemand, type VideoOutput, type VideoOutputEvents } from '#ports/Video.ts';
import { readJsonLines, writeJsonLine } from '../process/JsonLines.ts';
import { ManagedChildProcess } from '../process/ManagedChildProcess.ts';
import { HubEventSchema, type HubEvent } from './BridgeNativeSchemas.ts';

export interface VideoHubOptions {
  readonly executablePath: string;
  readonly logger: Logger;
  /** Pre-rendered NV12 placeholder frames by kind, all of `placeholderSize`. */
  readonly placeholders: ReadonlyMap<PlaceholderKind, string>;
  readonly placeholderSize: VideoSize;
  /** Overrides for tests; production uses the installed default mode, cap and pipe name. */
  readonly defaultMode?: VideoMode;
  readonly cap?: ModeCap;
  readonly pipeName?: string;
  readonly readyTimeoutMs?: number;
  readonly ingestModeTimeoutMs?: number;
}

interface IngestModeEvents {
  acknowledged: [size: VideoSize];
}

/**
 * The virtual camera endpoint: a long-running `bridge-native video hub` process that receives
 * raw frames from the decoder on a private ingest pipe and serves them, scaled, to camera
 * consumers. Its consumers and their modes are the video demand signal.
 */
export class VideoHubOutput implements VideoOutput {
  readonly #process: ManagedChildProcess;
  readonly #events = new Emitter<VideoOutputEvents>();
  readonly #ingestModeEvents = new Emitter<IngestModeEvents>();
  readonly #logger: Logger;
  readonly #ingestPath: string;
  readonly #ingestModeTimeoutMs: number;
  #defaultMode: VideoMode | undefined;
  #ingestMode: VideoSize | undefined;
  #consumers: ConsumerDemand = NO_CONSUMERS;
  #placeholder: PlaceholderKind | null | undefined;
  #disposing = false;

  private constructor(child: ManagedChildProcess, ingestPath: string, logger: Logger, ingestModeTimeoutMs: number) {
    this.#process = child;
    this.#ingestPath = ingestPath;
    this.#logger = logger;
    this.#ingestModeTimeoutMs = ingestModeTimeoutMs;
  }

  static async start(options: VideoHubOptions): Promise<VideoHubOutput> {
    const logger = options.logger.child({ component: 'video-hub' });
    const ingestPath = `\\\\.\\pipe\\mobile-webcam-bridge-ingest-${randomBytes(8).toString('hex')}`;
    const args = ['video', 'hub', '--ingest-pipe', ingestPath];
    if (options.pipeName !== undefined) args.push('--pipe-name', options.pipeName);
    if (options.defaultMode !== undefined) {
      const { width, height, fpsNum, fpsDen } = options.defaultMode;
      args.push(
        '--width',
        String(width),
        '--height',
        String(height),
        '--fps-num',
        String(fpsNum),
        '--fps-den',
        String(fpsDen),
      );
    }
    if (options.cap !== undefined) {
      const { maxWidth, maxHeight, maxFps } = options.cap;
      args.push('--max-width', String(maxWidth), '--max-height', String(maxHeight), '--max-fps', String(maxFps));
    }
    const child = ManagedChildProcess.spawn({ command: options.executablePath, args, logger, stderrLevel: 'info' });
    const hub = new VideoHubOutput(child, ingestPath, logger, options.ingestModeTimeoutMs ?? 2000);
    await hub.#waitUntilReady(options.readyTimeoutMs ?? 5000);
    const { width, height } = options.placeholderSize;
    for (const [kind, path] of options.placeholders) {
      writeJsonLine(child.stdin, { cmd: 'loadPlaceholder', kind, width, height, path });
    }
    return hub;
  }

  get ingestPath(): string {
    return this.#ingestPath;
  }

  get defaultMode(): VideoMode {
    if (this.#defaultMode === undefined) throw new Error('Video hub is not ready');
    return this.#defaultMode;
  }

  get ingestMode(): VideoSize {
    if (this.#ingestMode === undefined) throw new Error('Video hub is not ready');
    return this.#ingestMode;
  }

  get consumers(): ConsumerDemand {
    return this.#consumers;
  }

  get events(): EventSource<VideoOutputEvents> {
    return this.#events;
  }

  setPlaceholder(kind: PlaceholderKind | null): void {
    if (kind === this.#placeholder || this.#process.hasExited) return;
    this.#placeholder = kind;
    writeJsonLine(this.#process.stdin, { cmd: 'placeholder', kind });
  }

  async setIngestMode(size: VideoSize, signal: AbortSignal): Promise<void> {
    signal.throwIfAborted();
    if (this.#process.hasExited) throw new MediaError('NATIVE_HELPER_FAILED', 'Video hub is not running');
    using cleanup = new DisposableStack();
    const acknowledged = new Promise<void>((resolve, reject) => {
      cleanup.use(
        this.#ingestModeEvents.on('acknowledged', (acknowledgedSize) => {
          if (sameSize(acknowledgedSize, size)) resolve();
        }),
      );
      cleanup.use(
        this.#events.on('failed', (error) => {
          reject(error);
        }),
      );
      const timer = setTimeout(() => {
        reject(
          new MediaError(
            'NATIVE_HELPER_FAILED',
            `Video hub did not switch the ingest size to ${describeSize(size)} within ${this.#ingestModeTimeoutMs} ms`,
          ),
        );
      }, this.#ingestModeTimeoutMs);
      cleanup.defer(() => {
        clearTimeout(timer);
      });
      const onAbort = (): void => {
        reject(abortReason(signal));
      };
      signal.addEventListener('abort', onAbort, { once: true });
      cleanup.defer(() => {
        signal.removeEventListener('abort', onAbort);
      });
    });
    writeJsonLine(this.#process.stdin, { cmd: 'ingest', width: size.width, height: size.height });
    await acknowledged;
  }

  async [Symbol.asyncDispose](): Promise<void> {
    this.#disposing = true;
    await this.#process.stop(1500);
    this.#events.clear();
    this.#ingestModeEvents.clear();
  }

  #waitUntilReady(timeoutMs: number): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      const timer = setTimeout(() => {
        void this.#process.stop(0);
        reject(new MediaError('NATIVE_HELPER_FAILED', `Video hub did not become ready within ${timeoutMs} ms`));
      }, timeoutMs);
      let ready = false;
      readJsonLines(
        this.#process.stdout,
        HubEventSchema,
        (event) => {
          if (event.event === 'ready' && !ready) {
            ready = true;
            clearTimeout(timer);
            this.#defaultMode = {
              width: event.width,
              height: event.height,
              fpsNum: event.fpsNum,
              fpsDen: event.fpsDen,
            };
            this.#ingestMode = { width: event.width, height: event.height };
            this.#logger.info('Video hub ready', {
              publicPipe: event.publicPipe,
              defaultMode: describeMode(this.#defaultMode),
              cap: `${event.maxWidth}x${event.maxHeight}@${event.maxFps}`,
            });
            resolve();
            return;
          }
          if (event.event === 'error' && !ready && event.fatal) {
            clearTimeout(timer);
            reject(new MediaError(event.code === 'PIPE_IN_USE' ? 'HUB_IN_USE' : 'NATIVE_HELPER_FAILED', event.message));
            return;
          }
          this.#handleEvent(event);
        },
        (line, reason) => {
          this.#logger.warn('Unexpected video hub output', { line, reason });
        },
      );
      void this.#process.exited.then((exit) => {
        clearTimeout(timer);
        if (!ready) {
          reject(
            new MediaError(
              exit.spawnError === undefined ? 'NATIVE_HELPER_FAILED' : 'NATIVE_NOT_INSTALLED',
              `Video hub exited before becoming ready (code ${exit.code ?? 'none'})`,
              { cause: exit.spawnError },
            ),
          );
          return;
        }
        if (!this.#disposing) {
          this.#setConsumers(NO_CONSUMERS);
          this.#events.emit(
            'failed',
            new MediaError('NATIVE_HELPER_FAILED', `Video hub exited (code ${exit.code ?? 'none'})`),
          );
        }
      });
    });
  }

  #handleEvent(event: HubEvent): void {
    switch (event.event) {
      case 'consumers':
        this.#setConsumers({ count: event.count, modes: event.modes });
        break;
      case 'ingestMode': {
        const size = { width: event.width, height: event.height };
        this.#ingestMode = size;
        this.#logger.debug('Ingest size', { size: describeSize(size) });
        this.#ingestModeEvents.emit('acknowledged', size);
        break;
      }
      case 'ingest':
        this.#logger.debug('Decoder connection', { connected: event.connected });
        break;
      case 'stats':
        this.#logger.debug('Video hub stats', { ...event });
        break;
      case 'error':
        this.#logger.warn('Video hub error', { code: event.code, message: event.message, fatal: event.fatal });
        break;
      case 'ready':
        break;
    }
  }

  #setConsumers(demand: ConsumerDemand): void {
    const current = this.#consumers;
    const unchanged =
      demand.count === current.count &&
      demand.modes.every((mode, index) => {
        const other = current.modes[index];
        return other !== undefined && sameMode(mode, other);
      });
    if (unchanged) return;
    this.#consumers = demand;
    this.#logger.info('Camera consumers', { count: demand.count, modes: demand.modes.map(describeMode) });
    this.#events.emit('consumersChanged', demand);
  }
}

import { randomBytes } from 'node:crypto';
import { MediaError } from '#domain/errors.ts';
import { Emitter, type EventSource } from '#domain/events.ts';
import type { PlaceholderKind } from '#domain/video/PlaceholderPolicy.ts';
import type { VideoMode } from '#domain/video/VideoMode.ts';
import type { Logger } from '#ports/Logger.ts';
import type { VideoOutput, VideoOutputEvents } from '#ports/Video.ts';
import { readJsonLines, writeJsonLine } from '../process/JsonLines.ts';
import { ManagedChildProcess } from '../process/ManagedChildProcess.ts';
import { HubEventSchema, type HubEvent } from './BridgeNativeSchemas.ts';

export interface VideoHubOptions {
  readonly executablePath: string;
  readonly logger: Logger;
  /** Pre-rendered NV12 placeholder frames by kind. */
  readonly placeholders: ReadonlyMap<PlaceholderKind, string>;
  /** Overrides for tests; production uses the installed camera mode and pipe name. */
  readonly mode?: VideoMode;
  readonly pipeName?: string;
  readonly readyTimeoutMs?: number;
}

/**
 * The virtual camera endpoint: a long-running `bridge-native video hub` process that receives
 * raw frames from the decoder on a private ingest pipe and serves them to camera consumers.
 * Its consumer count is the video demand signal.
 */
export class VideoHubOutput implements VideoOutput {
  readonly #process: ManagedChildProcess;
  readonly #events = new Emitter<VideoOutputEvents>();
  readonly #logger: Logger;
  readonly #ingestPath: string;
  #mode: VideoMode | undefined;
  #consumers = 0;
  #placeholder: PlaceholderKind | null | undefined;
  #disposing = false;

  private constructor(child: ManagedChildProcess, ingestPath: string, logger: Logger) {
    this.#process = child;
    this.#ingestPath = ingestPath;
    this.#logger = logger;
  }

  static async start(options: VideoHubOptions): Promise<VideoHubOutput> {
    const logger = options.logger.child({ component: 'video-hub' });
    const ingestPath = `\\\\.\\pipe\\mobile-webcam-bridge-ingest-${randomBytes(8).toString('hex')}`;
    const args = ['video', 'hub', '--ingest-pipe', ingestPath];
    if (options.pipeName !== undefined) args.push('--pipe-name', options.pipeName);
    if (options.mode !== undefined) {
      const { width, height, fpsNum, fpsDen } = options.mode;
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
    const child = ManagedChildProcess.spawn({ command: options.executablePath, args, logger, stderrLevel: 'info' });
    const hub = new VideoHubOutput(child, ingestPath, logger);
    await hub.#waitUntilReady(options.readyTimeoutMs ?? 5000);
    for (const [kind, path] of options.placeholders) writeJsonLine(child.stdin, { cmd: 'loadPlaceholder', kind, path });
    return hub;
  }

  get ingestPath(): string {
    return this.#ingestPath;
  }

  get mode(): VideoMode {
    if (this.#mode === undefined) throw new Error('Video hub is not ready');
    return this.#mode;
  }

  get consumers(): number {
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

  async [Symbol.asyncDispose](): Promise<void> {
    this.#disposing = true;
    await this.#process.stop(1500);
    this.#events.clear();
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
            this.#mode = { width: event.width, height: event.height, fpsNum: event.fpsNum, fpsDen: event.fpsDen };
            this.#logger.info('Video hub ready', { publicPipe: event.publicPipe });
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
          this.#setConsumers(0);
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
        this.#setConsumers(event.count);
        break;
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

  #setConsumers(count: number): void {
    if (count === this.#consumers) return;
    this.#consumers = count;
    this.#logger.info('Camera consumers', { count });
    this.#events.emit('consumersChanged', count);
  }
}

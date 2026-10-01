import { MediaError } from '#domain/errors.ts';
import { Emitter, type EventSource } from '#domain/events.ts';
import type { AudioSink, AudioSinkEvents } from '#ports/Audio.ts';
import type { Logger } from '#ports/Logger.ts';
import { readJsonLines } from '../process/JsonLines.ts';
import { ManagedChildProcess } from '../process/ManagedChildProcess.ts';
import { MicEventSchema, NativeExitCode } from './BridgeNativeSchemas.ts';

export interface MicSinkOptions {
  readonly executablePath: string;
  readonly logger: Logger;
  /** Audio queued in Node beyond what the helper accepted; the oldest is dropped past this. */
  readonly maxQueueMs?: number;
  readonly readyTimeoutMs?: number;
}

const SAMPLE_RATE = 48_000;
const BYTES_PER_MS = (SAMPLE_RATE * 2) / 1000;

/**
 * The virtual microphone: a long-running `bridge-native mic feed` process that pushes PCM from
 * its stdin into mwbmic.sys and reports the driver's buffer state every 100 ms.
 */
export class BridgeNativeMicSink implements AudioSink {
  readonly #process: ManagedChildProcess;
  readonly #events = new Emitter<AudioSinkEvents>();
  readonly #logger: Logger;
  readonly #maxQueueBytes: number;
  readonly #queue: Buffer[] = [];
  #queuedBytes = 0;
  #disposing = false;

  private constructor(child: ManagedChildProcess, logger: Logger, maxQueueMs: number) {
    this.#process = child;
    this.#logger = logger;
    this.#maxQueueBytes = Math.round(maxQueueMs * BYTES_PER_MS);
    child.stdin.on('drain', () => {
      this.#flush();
    });
  }

  static async start(options: MicSinkOptions): Promise<BridgeNativeMicSink> {
    const logger = options.logger.child({ component: 'mic-feed' });
    const child = ManagedChildProcess.spawn({
      command: options.executablePath,
      args: ['mic', 'feed'],
      logger,
      stderrLevel: 'info',
    });
    const sink = new BridgeNativeMicSink(child, logger, options.maxQueueMs ?? 200);
    await sink.#waitUntilReady(options.readyTimeoutMs ?? 5000);
    return sink;
  }

  get events(): EventSource<AudioSinkEvents> {
    return this.#events;
  }

  get sampleRate(): number {
    return SAMPLE_RATE;
  }

  write(samples: Int16Array): boolean {
    if (this.#disposing || this.#process.hasExited || samples.length === 0) return false;
    const bytes = Buffer.from(samples.buffer, samples.byteOffset, samples.byteLength);
    if (this.#queue.length === 0 && !this.#process.stdin.writableNeedDrain) {
      this.#process.stdin.write(bytes);
      return true;
    }
    this.#queue.push(bytes);
    this.#queuedBytes += bytes.length;
    let dropped = false;
    while (this.#queuedBytes > this.#maxQueueBytes && this.#queue.length > 1) {
      const oldest = this.#queue.shift();
      this.#queuedBytes -= oldest?.length ?? 0;
      dropped = true;
    }
    return !dropped;
  }

  async [Symbol.asyncDispose](): Promise<void> {
    this.#disposing = true;
    this.#queue.length = 0;
    await this.#process.stop(1000);
    this.#events.clear();
  }

  #flush(): void {
    while (this.#queue.length > 0 && !this.#process.stdin.writableNeedDrain) {
      const next = this.#queue.shift();
      if (next === undefined) break;
      this.#queuedBytes -= next.length;
      this.#process.stdin.write(next);
    }
  }

  #waitUntilReady(timeoutMs: number): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      let ready = false;
      let startupError: string | undefined;
      const timer = setTimeout(() => {
        void this.#process.stop(0);
        reject(new MediaError('NATIVE_HELPER_FAILED', `Mic feeder did not become ready within ${timeoutMs} ms`));
      }, timeoutMs);
      readJsonLines(
        this.#process.stdout,
        MicEventSchema,
        (event) => {
          switch (event.event) {
            case 'ready':
              ready = true;
              clearTimeout(timer);
              resolve();
              break;
            case 'status':
              this.#events.emit('status', {
                bufferedBytes: event.bufferedBytes,
                capacityBytes: event.capacityBytes,
                streamActive: event.streamActive,
                underruns: event.underruns,
                overruns: event.overruns,
              });
              break;
            case 'error':
              if (!ready) startupError = `${event.code}: ${event.message}`;
              else this.#logger.warn('Mic feeder error', { code: event.code, message: event.message });
              break;
          }
        },
        (line, reason) => {
          this.#logger.warn('Unexpected mic feeder output', { line, reason });
        },
      );
      void this.#process.exited.then((exit) => {
        clearTimeout(timer);
        if (!ready) {
          const notInstalled = exit.spawnError !== undefined || exit.code === NativeExitCode.NotInstalled;
          reject(
            new MediaError(
              notInstalled ? 'NATIVE_NOT_INSTALLED' : 'AUDIO_SINK_FAILED',
              startupError ?? `Mic feeder exited before becoming ready (code ${exit.code ?? 'none'})`,
              { cause: exit.spawnError },
            ),
          );
        } else if (!this.#disposing) {
          this.#events.emit(
            'failed',
            new MediaError('AUDIO_SINK_FAILED', `Mic feeder exited (code ${exit.code ?? 'none'})`),
          );
        }
      });
    });
  }
}

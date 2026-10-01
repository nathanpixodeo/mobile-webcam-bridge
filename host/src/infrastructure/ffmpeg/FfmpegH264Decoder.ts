import { MediaError } from '#domain/errors.ts';
import { Emitter, type EventSource } from '#domain/events.ts';
import { ACCESS_UNIT_DELIMITER } from '#domain/h264/AnnexB.ts';
import type { Logger } from '#ports/Logger.ts';
import type {
  EncodedAccessUnit,
  VideoDecoder,
  VideoDecoderEvents,
  VideoDecoderFactory,
  VideoOutput,
} from '#ports/Video.ts';
import { ManagedChildProcess } from '../process/ManagedChildProcess.ts';
import { buildDecoderArgs, type HardwareAcceleration } from './FfmpegArgsBuilder.ts';

export interface FfmpegDecoderOptions {
  readonly ffmpegPath: string;
  readonly hwaccel: HardwareAcceleration;
  readonly logger: Logger;
  /** stdin backlog above which access units are refused (decoder congested). */
  readonly maxBufferedBytes?: number;
}

const DEFAULT_MAX_BUFFERED_BYTES = 1024 * 1024;

/** H.264 → NV12 decoder backed by an ffmpeg child process writing into the hub's ingest pipe. */
export class FfmpegH264Decoder implements VideoDecoder {
  readonly #process: ManagedChildProcess;
  readonly #events = new Emitter<VideoDecoderEvents>();
  readonly #maxBufferedBytes: number;
  #disposing = false;

  private constructor(child: ManagedChildProcess, maxBufferedBytes: number) {
    this.#process = child;
    this.#maxBufferedBytes = maxBufferedBytes;
    void child.exited.then((exit) => {
      if (this.#disposing) return;
      const reason =
        exit.spawnError !== undefined
          ? `ffmpeg could not start: ${exit.spawnError.message}`
          : `ffmpeg exited unexpectedly (code ${exit.code ?? 'none'}, signal ${exit.signal ?? 'none'})`;
      this.#events.emit(
        'failed',
        new MediaError(exit.spawnError === undefined ? 'DECODER_FAILED' : 'FFMPEG_NOT_FOUND', reason, {
          retryable: true,
        }),
      );
    });
  }

  static start(output: VideoOutput, options: FfmpegDecoderOptions): FfmpegH264Decoder {
    const args = buildDecoderArgs({ mode: output.mode, output: output.ingestPath, hwaccel: options.hwaccel });
    const child = ManagedChildProcess.spawn({
      command: options.ffmpegPath,
      args,
      logger: options.logger.child({ component: 'ffmpeg' }),
      stderrLevel: 'warn',
    });
    options.logger.debug('Decoder started', { pid: child.pid, output: output.ingestPath });
    return new FfmpegH264Decoder(child, options.maxBufferedBytes ?? DEFAULT_MAX_BUFFERED_BYTES);
  }

  get events(): EventSource<VideoDecoderEvents> {
    return this.#events;
  }

  decode(accessUnit: EncodedAccessUnit): boolean {
    const stdin = this.#process.stdin;
    if (this.#disposing || this.#process.hasExited || !stdin.writable) return false;
    if (stdin.writableLength > this.#maxBufferedBytes) return false;
    stdin.write(accessUnit.data);
    stdin.write(ACCESS_UNIT_DELIMITER);
    return true;
  }

  async [Symbol.asyncDispose](): Promise<void> {
    this.#disposing = true;
    await this.#process.stop(1000);
    this.#events.clear();
  }
}

export class FfmpegDecoderFactory implements VideoDecoderFactory {
  readonly #options: FfmpegDecoderOptions;

  constructor(options: FfmpegDecoderOptions) {
    this.#options = options;
  }

  async create(output: VideoOutput, signal: AbortSignal): Promise<VideoDecoder> {
    signal.throwIfAborted();
    await Promise.resolve();
    return FfmpegH264Decoder.start(output, this.#options);
  }
}

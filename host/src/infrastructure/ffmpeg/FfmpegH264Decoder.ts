import { MediaError } from '#domain/errors.ts';
import { Emitter, type EventSource } from '#domain/events.ts';
import { ACCESS_UNIT_DELIMITER } from '#domain/h264/AnnexB.ts';
import { describeMode, type VideoMode, type VideoSize } from '#domain/video/VideoMode.ts';
import type { Logger } from '#ports/Logger.ts';
import type {
  EncodedAccessUnit,
  VideoDecoder,
  VideoDecoderEvents,
  VideoDecoderFactory,
  VideoOutput,
} from '#ports/Video.ts';
import { ManagedChildProcess } from '../process/ManagedChildProcess.ts';
import {
  buildDecoderArgs,
  resolveHardwareAcceleration,
  type HardwareAcceleration,
  type HardwareAccelerationSetting,
} from './FfmpegArgsBuilder.ts';

export interface FfmpegDecoderOptions {
  readonly ffmpegPath: string;
  readonly hwaccel: HardwareAccelerationSetting;
  readonly logger: Logger;
}

/**
 * stdin backlog above which access units are refused (decoder congested). Above 1080p a single
 * IDR can exceed 1 MiB: with that limit the access units right after it would be refused while
 * ffmpeg still reads it, and every GOP would fall back to waiting for the next IDR.
 */
export function maxBacklogBytesFor(mode: VideoSize): number {
  return mode.height > 1080 ? 8 * 1024 * 1024 : 1024 * 1024;
}

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

  static start(
    output: VideoOutput,
    mode: VideoMode,
    hwaccel: HardwareAcceleration,
    options: Pick<FfmpegDecoderOptions, 'ffmpegPath' | 'logger'>,
  ): FfmpegH264Decoder {
    const args = buildDecoderArgs({ mode, output: output.ingestPath, hwaccel });
    const child = ManagedChildProcess.spawn({
      command: options.ffmpegPath,
      args,
      logger: options.logger.child({ component: 'ffmpeg' }),
      stderrLevel: 'warn',
    });
    options.logger.debug('Decoder started', {
      pid: child.pid,
      output: output.ingestPath,
      mode: describeMode(mode),
      hwaccel,
    });
    return new FfmpegH264Decoder(child, maxBacklogBytesFor(mode));
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
  /** Set once a hardware decoder failed: the rest of the run decodes in software. */
  #softwareOnly = false;

  constructor(options: FfmpegDecoderOptions) {
    this.#options = options;
  }

  get softwareOnly(): boolean {
    return this.#softwareOnly;
  }

  async create(output: VideoOutput, mode: VideoMode, signal: AbortSignal): Promise<VideoDecoder> {
    signal.throwIfAborted();
    await Promise.resolve();
    const hwaccel = this.#softwareOnly ? 'none' : resolveHardwareAcceleration(this.#options.hwaccel, mode);
    const decoder = FfmpegH264Decoder.start(output, mode, hwaccel, this.#options);
    if (hwaccel !== 'none') {
      decoder.events.on('failed', (error) => {
        if (this.#softwareOnly) return;
        this.#softwareOnly = true;
        this.#options.logger.warn('Hardware decoding failed; decoding in software from now on', {
          hwaccel,
          error: error.message,
        });
      });
    }
    return decoder;
  }
}

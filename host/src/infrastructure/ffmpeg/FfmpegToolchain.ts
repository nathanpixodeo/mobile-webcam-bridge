import { mkdir, stat } from 'node:fs/promises';
import { join } from 'node:path';
import { MediaError } from '#domain/errors.ts';
import type { PlaceholderKind } from '#domain/video/PlaceholderPolicy.ts';
import { nv12FrameBytes, type VideoSize } from '#domain/video/VideoMode.ts';
import type { Logger } from '#ports/Logger.ts';
import { runToCompletion } from '../process/ManagedChildProcess.ts';
import { buildPlaceholderArgs } from './FfmpegArgsBuilder.ts';

/** Locates and probes the ffmpeg executable. */
export class FfmpegLocator {
  readonly #path: string;
  readonly #logger: Logger;

  constructor(path: string, logger: Logger) {
    this.#path = path;
    this.#logger = logger;
  }

  get path(): string {
    return this.#path;
  }

  /** Returns the first line of `ffmpeg -version`, or throws FFMPEG_NOT_FOUND. */
  async version(): Promise<string> {
    try {
      const { exit, stdout } = await runToCompletion(this.#path, ['-hide_banner', '-version'], {
        timeoutMs: 10_000,
        logger: this.#logger,
      });
      if (exit.spawnError !== undefined || exit.code !== 0)
        throw exit.spawnError ?? new Error(`exit code ${exit.code}`);
      return stdout.split(/\r?\n/, 1)[0] ?? 'ffmpeg';
    } catch (error) {
      throw new MediaError(
        'FFMPEG_NOT_FOUND',
        `ffmpeg not usable at "${this.#path}". Install ffmpeg or set ffmpeg.path.`,
        {
          cause: error,
        },
      );
    }
  }
}

/** Size of the shipped placeholder PNGs, kept for the NV12 frames: the hub scales them per consumer. */
export const PLACEHOLDER_FRAME_SIZE: VideoSize = { width: 1920, height: 1080 };

/**
 * Converts the placeholder PNGs into raw NV12 frames of `PLACEHOLDER_FRAME_SIZE`, once, into a
 * cache directory (re-rendered when the PNG is newer than the cached frame).
 */
export class FfmpegPlaceholderRenderer {
  readonly #ffmpegPath: string;
  readonly #assetsDir: string;
  readonly #cacheDir: string;
  readonly #logger: Logger;

  constructor(options: { ffmpegPath: string; assetsDir: string; cacheDir: string; logger: Logger }) {
    this.#ffmpegPath = options.ffmpegPath;
    this.#assetsDir = options.assetsDir;
    this.#cacheDir = options.cacheDir;
    this.#logger = options.logger;
  }

  async render(kinds: readonly PlaceholderKind[]): Promise<Map<PlaceholderKind, string>> {
    await mkdir(this.#cacheDir, { recursive: true });
    const result = new Map<PlaceholderKind, string>();
    const { width, height } = PLACEHOLDER_FRAME_SIZE;
    for (const kind of kinds) {
      const input = join(this.#assetsDir, `${kind}.png`);
      const output = join(this.#cacheDir, `${kind}-${width}x${height}.nv12`);
      try {
        if (!(await this.#isFresh(input, output))) await this.#renderOne(input, output);
        result.set(kind, output);
      } catch (error) {
        this.#logger.warn('Placeholder not available', { kind, error: (error as Error).message });
      }
    }
    return result;
  }

  async #isFresh(input: string, output: string): Promise<boolean> {
    try {
      const [source, cached] = await Promise.all([stat(input), stat(output)]);
      return cached.size === nv12FrameBytes(PLACEHOLDER_FRAME_SIZE) && cached.mtimeMs >= source.mtimeMs;
    } catch {
      return false;
    }
  }

  async #renderOne(input: string, output: string): Promise<void> {
    const args = buildPlaceholderArgs({ input, mode: PLACEHOLDER_FRAME_SIZE, output });
    const { exit } = await runToCompletion(this.#ffmpegPath, args, { timeoutMs: 15_000, logger: this.#logger });
    if (exit.code !== 0) throw new Error(`ffmpeg failed to render ${input} (exit ${exit.code})`);
    const bytes = (await stat(output)).size;
    if (bytes !== nv12FrameBytes(PLACEHOLDER_FRAME_SIZE)) throw new Error(`rendered placeholder has ${bytes} bytes`);
  }
}

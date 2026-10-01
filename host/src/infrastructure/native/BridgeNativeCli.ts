import type * as z from 'zod';
import { InstallError, MediaError } from '#domain/errors.ts';
import type { Logger } from '#ports/Logger.ts';
import type {
  DoctorCheckResult,
  InstallOptions,
  InstallResult,
  NativeHelper,
  NativeStatus,
} from '#ports/NativeHelper.ts';
import { runToCompletion } from '../process/ManagedChildProcess.ts';
import {
  DoctorOutputSchema,
  InstallOutputSchema,
  NativeExitCode,
  NativeFailureSchema,
  StatusSchema,
} from './BridgeNativeSchemas.ts';

const ONE_SHOT_TIMEOUT_MS = 30_000;
/** Installs wait for the user to answer UAC and for driver installation. */
const INSTALL_TIMEOUT_MS = 10 * 60_000;

/** One-shot bridge-native.exe commands (protocol/BRIDGE_NATIVE.md §2). */
export class BridgeNativeCli implements NativeHelper {
  readonly #executablePath: string;
  readonly #logger: Logger;

  constructor(executablePath: string, logger: Logger) {
    this.#executablePath = executablePath;
    this.#logger = logger;
  }

  get executablePath(): string {
    return this.#executablePath;
  }

  async status(): Promise<NativeStatus> {
    const output = await this.#run(['status'], StatusSchema, ONE_SHOT_TIMEOUT_MS);
    return {
      installDir: output.installDir ?? null,
      osBuild: output.os.build,
      isWin11: output.os.isWin11,
      camera: output.camera.installed
        ? {
            installed: true,
            backend: output.camera.backend,
            friendlyName: output.camera.friendlyName,
            pipeName: output.camera.pipeName,
            mode: {
              width: output.camera.width,
              height: output.camera.height,
              fpsNum: output.camera.fpsNum,
              fpsDen: output.camera.fpsDen,
            },
          }
        : { installed: false },
      mic: output.mic,
    };
  }

  async install(options: InstallOptions): Promise<InstallResult> {
    const args = [
      'install',
      '--camera',
      options.camera,
      options.mic ? '--mic' : '--no-mic',
      '--width',
      String(options.mode.width),
      '--height',
      String(options.mode.height),
      '--fps',
      String(options.mode.fps),
      '--name',
      options.friendlyName,
    ];
    return this.#installLike(args);
  }

  async uninstall(scope: { readonly camera: boolean; readonly mic: boolean }): Promise<InstallResult> {
    const flags = [...(scope.camera ? ['--camera'] : []), ...(scope.mic ? ['--mic'] : [])];
    return this.#installLike(['uninstall', ...flags]);
  }

  async doctor(): Promise<readonly DoctorCheckResult[]> {
    const output = await this.#run(['doctor'], DoctorOutputSchema, ONE_SHOT_TIMEOUT_MS, { acceptFailure: true });
    return output.checks;
  }

  async #installLike(args: string[]): Promise<InstallResult> {
    const output = await this.#run(args, InstallOutputSchema, INSTALL_TIMEOUT_MS, { acceptFailure: true });
    return { ok: output.ok, installDir: output.installDir, steps: output.steps };
  }

  async #run<S extends z.ZodType>(
    args: string[],
    schema: S,
    timeoutMs: number,
    options: { readonly acceptFailure?: boolean } = {},
  ): Promise<z.infer<S>> {
    this.#logger.debug('bridge-native', { args });
    const { exit, stdout } = await runToCompletion(this.#executablePath, args, { timeoutMs, logger: this.#logger });
    if (exit.spawnError !== undefined) {
      throw new MediaError('NATIVE_NOT_INSTALLED', `Cannot run ${this.#executablePath}: ${exit.spawnError.message}`, {
        cause: exit.spawnError,
      });
    }
    let json: unknown;
    try {
      json = JSON.parse(stdout.trim());
    } catch (error) {
      throw new MediaError('NATIVE_HELPER_FAILED', `bridge-native ${args[0] ?? ''} printed invalid JSON`, {
        cause: error,
      });
    }
    const failure = NativeFailureSchema.safeParse(json);
    if (exit.code === NativeExitCode.ElevationCancelled) {
      throw new InstallError('ELEVATION_CANCELLED', 'Administrator permission was declined');
    }
    if (exit.code === NativeExitCode.NotSupportedOs) {
      throw new InstallError(
        'NOT_SUPPORTED_OS',
        failure.success ? failure.data.error.message : 'Operating system not supported',
      );
    }
    if (failure.success && options.acceptFailure !== true) {
      const code = exit.code === NativeExitCode.NotInstalled ? 'NATIVE_NOT_INSTALLED' : 'NATIVE_HELPER_FAILED';
      throw new MediaError(
        code,
        `bridge-native ${args[0] ?? ''}: ${failure.data.error.message} (${failure.data.error.code})`,
      );
    }
    const parsed = schema.safeParse(json);
    if (!parsed.success) {
      throw new MediaError(
        'NATIVE_HELPER_FAILED',
        `Unexpected bridge-native ${args[0] ?? ''} output: ${parsed.error.message}`,
      );
    }
    return parsed.data;
  }
}

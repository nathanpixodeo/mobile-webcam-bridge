import { spawn, type ChildProcess } from 'node:child_process';
import { existsSync } from 'node:fs';
import { delimiter, join } from 'node:path';
import { toError } from '#domain/errors.ts';
import type { Logger } from '#ports/Logger.ts';

const ADB_EXECUTABLE = 'adb.exe';
const START_TIMEOUT_MS = 15_000;

/**
 * Finds adb.exe, in order: the configured path, each `PATH` entry, the default Android Studio SDK
 * location, then `ANDROID_HOME` / `ANDROID_SDK_ROOT`.
 */
export function locateAdb(configured: string | undefined): string | undefined {
  if (configured !== undefined && existsSync(configured)) return configured;
  return [...pathCandidates(), ...sdkCandidates()].find((candidate) => existsSync(candidate));
}

function pathCandidates(): string[] {
  return (process.env['PATH'] ?? '')
    .split(delimiter)
    .map((entry) => entry.trim().replace(/^"(.*)"$/, '$1'))
    .filter((entry) => entry !== '')
    .map((entry) => join(entry, ADB_EXECUTABLE));
}

function sdkCandidates(): string[] {
  const localAppData = process.env['LOCALAPPDATA'];
  return [
    localAppData === undefined ? undefined : join(localAppData, 'Android', 'Sdk'),
    process.env['ANDROID_HOME'],
    process.env['ANDROID_SDK_ROOT'],
  ]
    .filter((root): root is string => root !== undefined && root !== '')
    .map((root) => join(root, 'platform-tools', ADB_EXECUTABLE));
}

export interface AdbServerLauncherOptions {
  readonly adbPath: string;
  readonly logger: Logger;
}

/** Starts the ADB server daemon through `adb start-server` when nothing listens on its port. */
export class AdbServerLauncher {
  readonly #options: AdbServerLauncherOptions;

  constructor(options: AdbServerLauncherOptions) {
    this.#options = options;
  }

  /**
   * Resolves true when `adb start-server` exited with code 0 in time; never rejects. stdio is
   * ignored rather than piped: the daemon adb forks inherits piped handles and would keep a
   * stdout 'end' from ever firing.
   */
  start(): Promise<boolean> {
    const { adbPath, logger } = this.#options;
    return new Promise<boolean>((resolve) => {
      let child: ChildProcess;
      try {
        child = spawn(adbPath, ['start-server'], { stdio: 'ignore', windowsHide: true });
      } catch (error) {
        logger.debug('Cannot run adb start-server', { adbPath, error: toError(error).message });
        resolve(false);
        return;
      }
      let timedOut = false;
      const timer = setTimeout(() => {
        timedOut = true;
        logger.warn('adb start-server did not finish in time, killing it', { adbPath, timeoutMs: START_TIMEOUT_MS });
        child.kill();
      }, START_TIMEOUT_MS);
      // Stays attached for the child's whole life: kill() can still emit 'error' after a timeout.
      child.on('error', (error) => {
        clearTimeout(timer);
        logger.debug('Cannot run adb start-server', { adbPath, error: error.message });
        resolve(false);
      });
      child.once('exit', (code) => {
        clearTimeout(timer);
        if (code !== 0 && !timedOut) logger.warn('adb start-server failed', { adbPath, code });
        resolve(code === 0);
      });
    });
  }
}

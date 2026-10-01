import { spawn, type ChildProcessByStdio } from 'node:child_process';
import { once } from 'node:events';
import { createInterface } from 'node:readline';
import type { Readable, Writable } from 'node:stream';
import { setTimeout as sleep } from 'node:timers/promises';
import type { Logger } from '#ports/Logger.ts';

export interface ProcessExit {
  readonly code: number | null;
  readonly signal: NodeJS.Signals | null;
  /** Set when the process could not be started at all (e.g. ENOENT). */
  readonly spawnError: Error | undefined;
}

export interface ManagedChildProcessOptions {
  readonly command: string;
  readonly args: readonly string[];
  readonly logger: Logger;
  /** Log level for stderr lines. */
  readonly stderrLevel?: 'debug' | 'info' | 'warn';
  readonly cwd?: string;
}

/**
 * A child process with piped stdio and a disciplined shutdown: `stop()` closes stdin (our
 * helpers exit on EOF), waits for a grace period, then kills. stderr is forwarded to the logger
 * line by line, and stdin write errors (EPIPE after a crash) never become uncaught exceptions.
 */
export class ManagedChildProcess implements AsyncDisposable {
  readonly #child: ChildProcessByStdio<Writable, Readable, Readable>;
  readonly #exited: Promise<ProcessExit>;
  readonly #logger: Logger;
  #exit: ProcessExit | undefined;

  private constructor(child: ChildProcessByStdio<Writable, Readable, Readable>, logger: Logger) {
    this.#child = child;
    this.#logger = logger;
    this.#exited = new Promise<ProcessExit>((resolve) => {
      child.once('error', (error) => {
        // 'error' without 'exit' means the process never started.
        if (child.pid === undefined) {
          this.#exit = { code: null, signal: null, spawnError: error };
          resolve(this.#exit);
        } else {
          logger.warn('Child process error', { error: error.message });
        }
      });
      child.once('exit', (code, signal) => {
        this.#exit = { code, signal, spawnError: undefined };
        resolve(this.#exit);
      });
    });
    child.stdin.on('error', (error) => {
      logger.debug('Child stdin closed', { error: error.message });
    });
  }

  static spawn(options: ManagedChildProcessOptions): ManagedChildProcess {
    const logger = options.logger;
    const child = spawn(options.command, options.args, {
      cwd: options.cwd,
      stdio: ['pipe', 'pipe', 'pipe'],
      windowsHide: true,
    });
    const managed = new ManagedChildProcess(child, logger);
    const level = options.stderrLevel ?? 'debug';
    createInterface({ input: child.stderr, crlfDelay: Number.POSITIVE_INFINITY }).on('line', (line) => {
      if (line.trim() !== '') logger[level](line);
    });
    return managed;
  }

  get pid(): number | undefined {
    return this.#child.pid;
  }

  get stdin(): Writable {
    return this.#child.stdin;
  }

  get stdout(): Readable {
    return this.#child.stdout;
  }

  get exited(): Promise<ProcessExit> {
    return this.#exited;
  }

  get hasExited(): boolean {
    return this.#exit !== undefined;
  }

  /** Closes stdin, waits up to `graceMs` for a voluntary exit, then kills the process. */
  async stop(graceMs = 1000): Promise<ProcessExit> {
    if (this.#exit !== undefined) return this.#exit;
    if (!this.#child.stdin.destroyed) this.#child.stdin.end();
    const exited = await Promise.race([this.#exited, sleep(graceMs).then(() => undefined)]);
    if (exited !== undefined) return exited;
    this.#logger.debug('Child did not exit after stdin EOF, killing', { pid: this.#child.pid });
    this.#child.kill();
    return this.#exited;
  }

  async [Symbol.asyncDispose](): Promise<void> {
    await this.stop();
  }
}

/** Runs a command to completion and returns its stdout (one-shot helpers). */
export async function runToCompletion(
  command: string,
  args: readonly string[],
  options: { readonly timeoutMs: number; readonly logger: Logger },
): Promise<{ readonly exit: ProcessExit; readonly stdout: string }> {
  const child = ManagedChildProcess.spawn({ command, args, logger: options.logger });
  child.stdin.end();
  const chunks: Buffer[] = [];
  child.stdout.on('data', (chunk: Buffer) => chunks.push(chunk));
  const timer = sleep(options.timeoutMs).then(() => 'timeout' as const);
  const result = await Promise.race([child.exited, timer]);
  if (result === 'timeout') {
    await child.stop(0);
    throw new Error(`${command} ${args.join(' ')} timed out after ${options.timeoutMs} ms`);
  }
  if (!child.stdout.readableEnded) await once(child.stdout, 'end').catch(() => undefined);
  return { exit: result, stdout: Buffer.concat(chunks).toString('utf8') };
}

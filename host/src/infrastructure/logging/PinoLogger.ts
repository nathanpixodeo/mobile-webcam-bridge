import { mkdirSync, readdirSync, statSync, unlinkSync } from 'node:fs';
import { join } from 'node:path';
import pino from 'pino';
import pretty from 'pino-pretty';
import type { LogFields, Logger } from '#ports/Logger.ts';

export interface LoggerOptions {
  readonly level: pino.LevelWithSilent;
  /** Directory for daily log files; omit to log to the console only. */
  readonly directory?: string;
  readonly retentionDays: number;
  /** Human-friendly console output (TTY); JSON lines otherwise. */
  readonly pretty: boolean;
}

/** Adapts pino to the `Logger` port. */
export class PinoLogger implements Logger {
  readonly #pino: pino.Logger;

  private constructor(instance: pino.Logger) {
    this.#pino = instance;
  }

  static create(options: LoggerOptions): PinoLogger {
    const streams: pino.StreamEntry[] = [
      {
        level: options.level === 'silent' ? 'fatal' : options.level,
        stream: options.pretty
          ? pretty({ colorize: true, translateTime: 'HH:MM:ss.l', ignore: 'pid,hostname', singleLine: true })
          : process.stdout,
      },
    ];
    if (options.directory !== undefined) {
      mkdirSync(options.directory, { recursive: true });
      deleteOldLogs(options.directory, options.retentionDays);
      const day = new Date().toISOString().slice(0, 10);
      streams.push({
        level: 'debug',
        stream: pino.destination({ dest: join(options.directory, `bridge-${day}.log`), mkdir: true, sync: false }),
      });
    }
    const instance = pino(
      { level: options.level === 'silent' ? 'silent' : 'debug', base: null },
      pino.multistream(streams, { dedupe: false }),
    );
    return new PinoLogger(instance);
  }

  child(bindings: LogFields): Logger {
    return new PinoLogger(this.#pino.child(bindings));
  }

  trace(message: string, fields?: LogFields): void {
    this.#pino.trace(fields ?? {}, message);
  }

  debug(message: string, fields?: LogFields): void {
    this.#pino.debug(fields ?? {}, message);
  }

  info(message: string, fields?: LogFields): void {
    this.#pino.info(fields ?? {}, message);
  }

  warn(message: string, fields?: LogFields): void {
    this.#pino.warn(fields ?? {}, message);
  }

  error(message: string, fields?: LogFields): void {
    this.#pino.error(fields ?? {}, message);
  }

  flush(): void {
    this.#pino.flush();
  }
}

function deleteOldLogs(directory: string, retentionDays: number): void {
  const cutoff = Date.now() - retentionDays * 24 * 60 * 60 * 1000;
  for (const name of readdirSync(directory)) {
    if (!/^bridge-\d{4}-\d{2}-\d{2}\.log$/.test(name)) continue;
    const path = join(directory, name);
    try {
      if (statSync(path).mtimeMs < cutoff) unlinkSync(path);
    } catch {
      // A log file locked by another instance is not worth failing startup for.
    }
  }
}

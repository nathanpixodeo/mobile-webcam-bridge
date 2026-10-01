import type { ParseArgsOptionsConfig } from 'node:util';
import type { Clock } from '#domain/clock.ts';
import type { Logger } from '#ports/Logger.ts';
import type { BridgeConfig } from '#infrastructure/config/ConfigSchema.ts';
import type { Terminal } from './Terminal.ts';

export type CommandOptions = Readonly<Record<string, string | boolean | undefined>>;

export interface CommandContext {
  readonly config: BridgeConfig;
  readonly configSource: string | undefined;
  readonly logger: Logger;
  readonly clock: Clock;
  readonly options: CommandOptions;
  readonly terminal: Terminal;
}

/** One CLI command. `run` resolves with the process exit code. */
export interface Command {
  readonly name: string;
  readonly summary: string;
  /** Command-specific options (merged with the global ones when parsing). */
  readonly options: ParseArgsOptionsConfig;
  /** Commands that run for a long time log to a file as well. */
  readonly longRunning?: boolean;
  run(context: CommandContext): Promise<number>;
}

export const ExitCode = {
  Ok: 0,
  Failure: 1,
  Usage: 2,
  Interrupted: 130,
} as const;

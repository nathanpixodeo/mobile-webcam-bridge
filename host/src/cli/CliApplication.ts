import { parseArgs, type ParseArgsOptionsConfig } from 'node:util';
import { isBridgeError, toError } from '#domain/errors.ts';
import type { Logger } from '#ports/Logger.ts';
import { HOST_VERSION } from '#composition/createBridgeApp.ts';
import { ConfigLoader } from '#infrastructure/config/ConfigLoader.ts';
import type { BridgeConfig } from '#infrastructure/config/ConfigSchema.ts';
import { PinoLogger } from '#infrastructure/logging/PinoLogger.ts';
import { AppPaths } from '#infrastructure/system/AppPaths.ts';
import { SystemClock } from '#infrastructure/system/SystemClock.ts';
import { ExitCode, type Command, type CommandOptions } from './Command.ts';
import { Terminal } from './Terminal.ts';

const GLOBAL_OPTIONS = {
  config: { type: 'string', short: 'c' },
  'log-level': { type: 'string' },
  device: { type: 'string' },
  tcp: { type: 'string' },
  help: { type: 'boolean', short: 'h' },
} as const satisfies ParseArgsOptionsConfig;

/** Parses the command line, loads configuration and logging, and dispatches to a command. */
export class CliApplication {
  readonly #commands: ReadonlyMap<string, Command>;
  readonly #terminal = new Terminal();

  constructor(commands: readonly Command[]) {
    this.#commands = new Map(commands.map((command) => [command.name, command]));
  }

  async run(argv: readonly string[]): Promise<number> {
    const [first, ...rest] = argv;
    if (first === '--version' || first === '-v') {
      this.#terminal.line(HOST_VERSION);
      return ExitCode.Ok;
    }
    if (first === undefined || first === '--help' || first === '-h' || first === 'help') {
      this.#printHelp();
      return ExitCode.Ok;
    }
    const command = first.startsWith('-') ? this.#commands.get('start') : this.#commands.get(first);
    if (command === undefined) {
      this.#terminal.error(`Unknown command "${first}".`);
      this.#printHelp();
      return ExitCode.Usage;
    }
    const args = first.startsWith('-') ? argv : rest;

    let options: CommandOptions;
    try {
      options = parseArgs({
        args: [...args],
        options: { ...GLOBAL_OPTIONS, ...command.options },
        allowPositionals: false,
        strict: true,
      }).values;
    } catch (error) {
      this.#terminal.error(toError(error).message);
      return ExitCode.Usage;
    }
    if (options['help'] === true) {
      this.#printCommandHelp(command);
      return ExitCode.Ok;
    }

    let config: BridgeConfig;
    let configSource: string | undefined;
    try {
      ({ config, source: configSource } = new ConfigLoader().load(asString(options['config']), overridesFrom(options)));
    } catch (error) {
      this.#terminal.error(toError(error).message);
      return ExitCode.Usage;
    }

    const logger = this.#createLogger(config, command);
    try {
      return await command.run({
        config,
        configSource,
        logger,
        clock: new SystemClock(),
        options,
        terminal: this.#terminal,
      });
    } catch (error) {
      const failure = toError(error);
      logger.error(failure.message, isBridgeError(failure) ? { code: failure.code } : { stack: failure.stack });
      return ExitCode.Failure;
    } finally {
      if (logger instanceof PinoLogger) logger.flush();
    }
  }

  #createLogger(config: BridgeConfig, command: Command): Logger {
    return PinoLogger.create({
      level: config.logging.level,
      pretty: process.stdout.isTTY,
      retentionDays: config.logging.retentionDays,
      ...(command.longRunning === true && config.logging.file ? { directory: AppPaths.logsDir() } : {}),
    });
  }

  #printHelp(): void {
    const t = this.#terminal;
    t.heading(
      `mobile-webcam-bridge ${HOST_VERSION} — use an iPhone or Android phone as a Windows webcam and microphone over USB`,
    );
    t.line();
    t.line('Usage: mobile-webcam-bridge <command> [options]');
    t.line();
    t.table(
      ['Command', 'Description'],
      [...this.#commands.values()].map((command) => [command.name, command.summary]),
    );
    t.line();
    t.line(
      'Global options: --config <file>  --log-level <level>  --device <udid|serial>  --tcp <host> (development)  --help',
    );
  }

  #printCommandHelp(command: Command): void {
    this.#terminal.heading(`mobile-webcam-bridge ${command.name} — ${command.summary}`);
    const names = Object.entries(command.options).map(
      ([name, spec]) => `--${name}${spec.type === 'string' ? ' <value>' : ''}`,
    );
    if (names.length > 0) this.#terminal.line(`Options: ${names.join('  ')}`);
  }
}

function asString(value: string | boolean | undefined): string | undefined {
  return typeof value === 'string' ? value : undefined;
}

function overridesFrom(options: CommandOptions): Record<string, unknown> {
  const overrides: Record<string, Record<string, unknown>> = {};
  const set = (section: string, key: string, value: unknown): void => {
    overrides[section] = { ...overrides[section], [key]: value };
  };
  const logLevel = asString(options['log-level']);
  if (logLevel !== undefined) set('logging', 'level', logLevel);
  const device = asString(options['device']);
  if (device !== undefined) set('device', 'id', device);
  const tcp = asString(options['tcp']);
  if (tcp !== undefined) set('usbmux', 'tcpHost', tcp);
  return overrides;
}

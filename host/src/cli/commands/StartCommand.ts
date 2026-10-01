import { createBridgeApp } from '#composition/createBridgeApp.ts';
import { ExitCode, type Command, type CommandContext } from '../Command.ts';

const SHUTDOWN_DEADLINE_MS = 5000;

/** Runs the bridge until Ctrl+C. */
export class StartCommand implements Command {
  readonly name = 'start';
  readonly summary = 'Run the bridge (default command)';
  readonly options = {};
  readonly longRunning = true;

  async run(context: CommandContext): Promise<number> {
    const { logger } = context;
    const abort = new AbortController();
    let interrupts = 0;
    const onSignal = (signal: NodeJS.Signals): void => {
      interrupts++;
      if (interrupts > 1) {
        logger.warn('Forced exit');
        process.exit(ExitCode.Interrupted);
      }
      logger.info('Stopping…', { signal });
      abort.abort();
    };
    const signals: NodeJS.Signals[] = ['SIGINT', 'SIGBREAK', 'SIGHUP', 'SIGTERM'];
    for (const signal of signals) process.on(signal, onSignal);

    try {
      const app = await createBridgeApp(context);
      try {
        await app.run(abort.signal);
      } finally {
        const deadline = setTimeout(() => {
          logger.warn('Shutdown took too long, exiting');
          process.exit(ExitCode.Failure);
        }, SHUTDOWN_DEADLINE_MS);
        deadline.unref();
        await app[Symbol.asyncDispose]();
        clearTimeout(deadline);
      }
      logger.info('Stopped');
      return abort.signal.aborted ? ExitCode.Ok : ExitCode.Failure;
    } finally {
      for (const signal of signals) process.off(signal, onSignal);
    }
  }
}

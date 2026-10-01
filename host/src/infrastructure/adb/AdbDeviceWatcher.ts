import { isAborted, whenAborted } from '#domain/abort.ts';
import type { Clock } from '#domain/clock.ts';
import { isAbortError, TransportError, toError } from '#domain/errors.ts';
import { Emitter, type EventSource } from '#domain/events.ts';
import { ExponentialBackoff } from '#domain/session/ExponentialBackoff.ts';
import type { AdbDevice, DeviceWatcher, DeviceWatcherEvents, PendingDevice } from '#ports/DeviceWatcher.ts';
import type { Logger } from '#ports/Logger.ts';
import type { AdbClient } from './AdbClient.ts';
import { adbLink, parseDeviceList, toAdbDevice, toPendingDevice, type AdbDeviceEntry } from './AdbProtocol.ts';
import type { AdbServerLauncher } from './AdbServerLauncher.ts';

export interface AdbDeviceWatcherOptions {
  readonly client: AdbClient;
  readonly clock: Clock;
  readonly logger: Logger;
  /** Starts the ADB server when it is not running. Omitted when adb.exe cannot be located. */
  readonly launcher?: AdbServerLauncher;
  /** Phones connected by wireless debugging are ignored unless enabled. */
  readonly includeNetworkDevices?: boolean;
}

/**
 * Tracks Android phones through the ADB server's `track-devices` subscription, which sends the
 * full device list immediately and again on every change. If the server goes away every device is
 * reported detached; the watcher starts it once per outage (when it can) and reconnects with
 * backoff until disposed.
 */
export class AdbDeviceWatcher implements DeviceWatcher {
  readonly #options: AdbDeviceWatcherOptions;
  readonly #events = new Emitter<DeviceWatcherEvents>();
  readonly #devices = new Map<string, AdbDevice>();
  readonly #abort = new AbortController();
  readonly #backoff = new ExponentialBackoff({ initialMs: 500, maxMs: 10_000, factor: 2, jitter: 0.2 });
  #pending: readonly PendingDevice[] = [];
  /** Serials already warned about, so each USB debugging prompt is reported once rather than once per list. */
  #warned: ReadonlySet<string> = new Set();
  /** `adb start-server` runs at most once per outage; a server that keeps failing is only retried with backoff. */
  #launcherTried = false;
  #loop: Promise<void> | undefined;

  constructor(options: AdbDeviceWatcherOptions) {
    this.#options = options;
  }

  get events(): EventSource<DeviceWatcherEvents> {
    return this.#events;
  }

  devices(): readonly AdbDevice[] {
    return [...this.#devices.values()];
  }

  pending(): readonly PendingDevice[] {
    return this.#pending;
  }

  async start(): Promise<void> {
    if (this.#loop !== undefined) return;
    let resolveInitial: () => void = () => undefined;
    const initial = new Promise<void>((resolve) => {
      resolveInitial = resolve;
    });
    this.#loop = this.#run(resolveInitial);
    await initial;
  }

  async [Symbol.asyncDispose](): Promise<void> {
    this.#abort.abort();
    await this.#loop;
    this.#events.clear();
  }

  async #run(initialised: () => void): Promise<void> {
    const signal = this.#abort.signal;
    const { clock, launcher, logger } = this.#options;
    while (!signal.aborted) {
      const failure = await this.#track(signal, initialised);
      if (failure === undefined) break;
      if (failure.code === 'ADB_UNAVAILABLE' && launcher !== undefined && !this.#launcherTried) {
        this.#launcherTried = true;
        logger.info('ADB server is not running, starting it');
        try {
          await Promise.race([launcher.start(), whenAborted(signal)]);
        } catch {
          break;
        }
        continue;
      }
      this.#events.emit('unavailable', failure);
      logger.debug('ADB server unavailable', { error: failure.message });
      initialised();
      this.#reset();
      try {
        await clock.sleep(this.#backoff.next(), signal);
      } catch {
        break;
      }
    }
    this.#reset();
  }

  /** Follows the device list until the connection fails: resolves with the failure, or undefined once aborted. */
  async #track(signal: AbortSignal, initialised: () => void): Promise<TransportError | undefined> {
    try {
      await using connection = await this.#options.client.trackDevices(signal);
      this.#backoff.reset();
      this.#launcherTried = false;
      for (;;) {
        this.#apply(parseDeviceList(await connection.readLengthPrefixed(signal)));
        initialised();
      }
    } catch (error) {
      if (isAborted(signal) || isAbortError(error)) return undefined;
      return error instanceof TransportError
        ? error
        : new TransportError('ADB_UNAVAILABLE', toError(error).message, { cause: error });
    }
  }

  /** Reconciles the tracked state with one device list: every list is the complete current picture. */
  #apply(entries: readonly AdbDeviceEntry[]): void {
    const ready = new Map<string, AdbDevice>();
    const pending: PendingDevice[] = [];
    for (const entry of entries) {
      if (this.#options.includeNetworkDevices !== true && adbLink(entry.serial) === 'network') continue;
      const device = toAdbDevice(entry);
      if (device !== undefined) {
        ready.set(device.id, device);
        continue;
      }
      const waiting = toPendingDevice(entry);
      if (waiting !== undefined) pending.push(waiting);
    }
    for (const device of ready.values()) this.#attach(device);
    for (const id of [...this.#devices.keys()]) {
      if (!ready.has(id)) this.#detach(id);
    }
    this.#pending = pending;
    this.#warnUnauthorized(pending);
  }

  #warnUnauthorized(pending: readonly PendingDevice[]): void {
    const unauthorized = new Set(
      pending.filter((device) => device.state === 'unauthorized').map((device) => device.id),
    );
    for (const id of unauthorized) {
      if (this.#warned.has(id)) continue;
      this.#options.logger.warn('Android device needs authorization', {
        id,
        hint: 'unlock the phone and accept the "Allow USB debugging" prompt',
      });
    }
    this.#warned = unauthorized;
  }

  #attach(device: AdbDevice): void {
    if (this.#devices.has(device.id)) return;
    this.#devices.set(device.id, device);
    this.#options.logger.info('Android device attached', { id: device.id, model: device.model });
    this.#events.emit('attached', device);
  }

  #detach(id: string): void {
    if (!this.#devices.delete(id)) return;
    this.#options.logger.info('Android device detached', { id });
    this.#events.emit('detached', id);
  }

  /** The ADB server is gone, and with it every device it knew about, ready or waiting. */
  #reset(): void {
    for (const id of [...this.#devices.keys()]) this.#detach(id);
    this.#pending = [];
    this.#warned = new Set();
  }
}

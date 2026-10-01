import type { Clock } from '#domain/clock.ts';
import { isAborted } from '#domain/abort.ts';
import { isAbortError, TransportError, toError } from '#domain/errors.ts';
import { Emitter, type EventSource } from '#domain/events.ts';
import { ExponentialBackoff } from '#domain/session/ExponentialBackoff.ts';
import type { DeviceWatcher, DeviceWatcherEvents, PendingDevice, UsbmuxDevice } from '#ports/DeviceWatcher.ts';
import type { Logger } from '#ports/Logger.ts';
import { parseDeviceEntry, type UsbmuxClient } from './UsbmuxClient.ts';
import type { UsbmuxConnection } from './UsbmuxConnection.ts';

export interface UsbmuxDeviceWatcherOptions {
  readonly client: UsbmuxClient;
  readonly clock: Clock;
  readonly logger: Logger;
  /** Network-connected devices (Wi-Fi sync) are ignored unless enabled. */
  readonly includeNetworkDevices?: boolean;
}

/**
 * Tracks attached iPhones through usbmuxd's `Listen` subscription. If usbmuxd goes away (the
 * Apple service restarts), every device is reported detached and the watcher reconnects with
 * backoff until disposed.
 */
export class UsbmuxDeviceWatcher implements DeviceWatcher {
  readonly #options: UsbmuxDeviceWatcherOptions;
  readonly #events = new Emitter<DeviceWatcherEvents>();
  readonly #devices = new Map<string, UsbmuxDevice>();
  readonly #abort = new AbortController();
  readonly #backoff = new ExponentialBackoff({ initialMs: 500, maxMs: 10_000, factor: 2, jitter: 0.2 });
  #loop: Promise<void> | undefined;

  constructor(options: UsbmuxDeviceWatcherOptions) {
    this.#options = options;
  }

  get events(): EventSource<DeviceWatcherEvents> {
    return this.#events;
  }

  devices(): readonly UsbmuxDevice[] {
    return [...this.#devices.values()];
  }

  /** usbmuxd lists untrusted iPhones as attached; trust problems surface when connecting. */
  pending(): readonly PendingDevice[] {
    return [];
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
    const { client, clock, logger } = this.#options;
    while (!signal.aborted) {
      let connection: UsbmuxConnection | undefined;
      try {
        // ListDevices first gives a definitive initial list; Listen then streams changes
        // (and repeats Attached for present devices, which `#attach` deduplicates).
        const present = await client.listDevices(signal);
        connection = await client.listen(signal);
        this.#backoff.reset();
        for (const device of present) this.#attach(device);
        initialised();
        const closed = new Promise<Error | undefined>((resolve) => {
          connection?.events.on('closed', resolve);
        });
        using _messages = connection.events.on('message', (message) => {
          this.#handleMessage(message);
        });
        const aborted = new Promise<undefined>((resolve) => {
          signal.addEventListener(
            'abort',
            () => {
              resolve(undefined);
            },
            { once: true },
          );
        });
        const error = await Promise.race([closed, aborted]);
        if (isAborted(signal)) break;
        logger.warn('usbmuxd connection lost', { error: error?.message });
      } catch (error) {
        if (isAborted(signal) || isAbortError(error)) break;
        const failure =
          error instanceof TransportError
            ? error
            : new TransportError('USBMUX_UNAVAILABLE', toError(error).message, { cause: error });
        this.#events.emit('unavailable', failure);
        logger.debug('usbmuxd unavailable', { error: failure.message });
        initialised();
      } finally {
        await connection?.[Symbol.asyncDispose]();
      }
      this.#detachAll();
      try {
        await clock.sleep(this.#backoff.next(), signal);
      } catch {
        break;
      }
    }
    this.#detachAll();
  }

  #handleMessage(message: Readonly<Record<string, unknown>>): void {
    switch (message['MessageType']) {
      case 'Attached': {
        const device = parseDeviceEntry(message as Parameters<typeof parseDeviceEntry>[0]);
        if (device !== undefined) this.#attach(device);
        break;
      }
      case 'Detached': {
        const id = message['DeviceID'];
        for (const device of this.#devices.values()) {
          if (device.muxDeviceId === id) this.#detach(device.id);
        }
        break;
      }
      default:
        break;
    }
  }

  #attach(device: UsbmuxDevice): void {
    if (device.link === 'network' && this.#options.includeNetworkDevices !== true) return;
    const known = this.#devices.get(device.id);
    if (known?.muxDeviceId === device.muxDeviceId) return;
    if (known !== undefined) this.#detach(known.id);
    this.#devices.set(device.id, device);
    this.#options.logger.info('iPhone attached', { id: device.id, muxDeviceId: device.muxDeviceId });
    this.#events.emit('attached', device);
  }

  #detach(id: string): void {
    if (!this.#devices.delete(id)) return;
    this.#options.logger.info('iPhone detached', { id });
    this.#events.emit('detached', id);
  }

  #detachAll(): void {
    for (const id of [...this.#devices.keys()]) this.#detach(id);
  }
}

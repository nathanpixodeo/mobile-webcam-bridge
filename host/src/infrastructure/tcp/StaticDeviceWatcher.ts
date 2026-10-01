import { Emitter, type EventSource } from '#domain/events.ts';
import type { DeviceWatcher, DeviceWatcherEvents, MobileDevice, PendingDevice } from '#ports/DeviceWatcher.ts';

const TCP_DEVICE: MobileDevice = {
  transport: 'tcp',
  platform: 'unknown',
  id: 'tcp-device',
  link: 'network',
  model: undefined,
};

/** A watcher that reports one fixed, always-attached device (tests and TCP development mode). */
export class StaticDeviceWatcher implements DeviceWatcher {
  readonly #events = new Emitter<DeviceWatcherEvents>();
  readonly #device: MobileDevice;
  #started = false;

  constructor(device: MobileDevice = TCP_DEVICE) {
    this.#device = device;
  }

  get events(): EventSource<DeviceWatcherEvents> {
    return this.#events;
  }

  devices(): readonly MobileDevice[] {
    return this.#started ? [this.#device] : [];
  }

  pending(): readonly PendingDevice[] {
    return [];
  }

  async start(): Promise<void> {
    if (this.#started) return;
    this.#started = true;
    await Promise.resolve();
    this.#events.emit('attached', this.#device);
  }

  /** Simulates unplugging (tests). */
  detach(): void {
    if (!this.#started) return;
    this.#started = false;
    this.#events.emit('detached', this.#device.id);
  }

  async [Symbol.asyncDispose](): Promise<void> {
    this.#events.clear();
    await Promise.resolve();
  }
}

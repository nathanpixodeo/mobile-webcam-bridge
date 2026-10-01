import { Emitter, type EventSource } from '#domain/events.ts';
import { describeDevice, sameAttachment, type DeviceWatcher, type MobileDevice } from '#ports/DeviceWatcher.ts';
import type { Logger } from '#ports/Logger.ts';
import type { DeviceSession } from './DeviceSession.ts';

export interface DeviceManagerEvents {
  sessionStarted: [session: DeviceSession];
  sessionEnded: [session: DeviceSession];
}

export interface DeviceManagerOptions {
  readonly watcher: DeviceWatcher;
  readonly createSession: (device: MobileDevice) => DeviceSession;
  readonly logger: Logger;
  /** Only this phone (iPhone UDID or Android serial) is used when set. */
  readonly pinnedId?: string | undefined;
}

/**
 * Chooses the active phone (the pinned one, or the first phone attached over USB, iPhone or
 * Android) and owns its `DeviceSession`. When the active phone detaches, the next one takes over.
 */
export class DeviceManager implements AsyncDisposable {
  readonly #options: DeviceManagerOptions;
  readonly #events = new Emitter<DeviceManagerEvents>();
  readonly #subscriptions = new DisposableStack();
  #active: DeviceSession | undefined;
  /** Serialises session switches. */
  #switching: Promise<void> = Promise.resolve();

  constructor(options: DeviceManagerOptions) {
    this.#options = options;
  }

  get events(): EventSource<DeviceManagerEvents> {
    return this.#events;
  }

  get activeSession(): DeviceSession | undefined {
    return this.#active;
  }

  async start(): Promise<void> {
    const { watcher, logger } = this.#options;
    this.#subscriptions.use(
      watcher.events.on('attached', () => {
        this.#reselect();
      }),
    );
    this.#subscriptions.use(
      watcher.events.on('detached', (id) => {
        if (this.#active?.device.id === id) this.#reselect();
      }),
    );
    await watcher.start();
    this.#reselect();
    if (watcher.devices().length === 0) {
      logger.info('Waiting for a phone: connect an iPhone (tap Trust) or an Android phone (allow USB debugging)');
    }
    for (const pending of watcher.pending()) {
      logger.warn('Phone connected but not ready', {
        id: pending.id,
        platform: pending.platform,
        state: pending.state,
      });
    }
  }

  async [Symbol.asyncDispose](): Promise<void> {
    this.#subscriptions.dispose();
    await this.#switching;
    await this.#switchTo(undefined);
  }

  #reselect(): void {
    this.#switching = this.#switching.then(() => this.#switchTo(this.#choose()));
  }

  #choose(): MobileDevice | undefined {
    const devices = this.#options.watcher.devices();
    const pinned = this.#options.pinnedId;
    if (pinned !== undefined) return devices.find((device) => device.id === pinned);
    const current = this.#active?.device;
    const stillThere = current === undefined ? undefined : devices.find((device) => sameAttachment(device, current));
    return stillThere ?? devices.find((device) => device.link === 'usb') ?? devices[0];
  }

  async #switchTo(device: MobileDevice | undefined): Promise<void> {
    const current = this.#active;
    if (device !== undefined && current !== undefined && sameAttachment(current.device, device)) return;
    if (current !== undefined) {
      this.#active = undefined;
      await current[Symbol.asyncDispose]();
      this.#events.emit('sessionEnded', current);
    }
    if (device !== undefined) {
      this.#options.logger.info('Using phone', {
        device: describeDevice(device),
        id: device.id,
        transport: device.transport,
      });
      const session = this.#options.createSession(device);
      this.#active = session;
      this.#events.emit('sessionStarted', session);
      session.start();
    }
  }
}

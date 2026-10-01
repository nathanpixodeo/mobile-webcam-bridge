import { Emitter, type EventSource } from '#domain/events.ts';
import type { DeviceWatcher, DeviceWatcherEvents, MobileDevice, PendingDevice } from '#ports/DeviceWatcher.ts';

/**
 * Presents several device watchers (usbmux for iPhones, ADB for Android phones) as one. Each
 * watcher keeps running independently; one service being unavailable never hides the others.
 */
export class CompositeDeviceWatcher implements DeviceWatcher {
  readonly #watchers: readonly DeviceWatcher[];
  readonly #events = new Emitter<DeviceWatcherEvents>();
  readonly #subscriptions = new DisposableStack();

  constructor(watchers: readonly DeviceWatcher[]) {
    this.#watchers = watchers;
    for (const watcher of watchers) {
      this.#subscriptions.use(
        watcher.events.on('attached', (device) => {
          this.#events.emit('attached', device);
        }),
      );
      this.#subscriptions.use(
        watcher.events.on('detached', (id) => {
          this.#events.emit('detached', id);
        }),
      );
      this.#subscriptions.use(
        watcher.events.on('unavailable', (error) => {
          this.#events.emit('unavailable', error);
        }),
      );
    }
  }

  get events(): EventSource<DeviceWatcherEvents> {
    return this.#events;
  }

  async start(): Promise<void> {
    await Promise.all(this.#watchers.map((watcher) => watcher.start()));
  }

  devices(): readonly MobileDevice[] {
    return this.#watchers.flatMap((watcher) => watcher.devices());
  }

  pending(): readonly PendingDevice[] {
    return this.#watchers.flatMap((watcher) => watcher.pending());
  }

  async [Symbol.asyncDispose](): Promise<void> {
    this.#subscriptions.dispose();
    await Promise.all(this.#watchers.map((watcher) => watcher[Symbol.asyncDispose]()));
    this.#events.clear();
  }
}

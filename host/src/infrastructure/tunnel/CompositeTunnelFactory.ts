import type { DeviceTunnel, DeviceTunnelFactory } from '#ports/DeviceTunnelFactory.ts';
import type { DeviceTransport, MobileDevice } from '#ports/DeviceWatcher.ts';

/** Routes each device to the tunnel factory of its transport (usbmux for iPhone, ADB for Android). */
export class CompositeTunnelFactory implements DeviceTunnelFactory {
  readonly #byTransport = new Map<DeviceTransport, DeviceTunnelFactory>();

  constructor(factories: readonly DeviceTunnelFactory[]) {
    for (const factory of factories) {
      for (const transport of factory.transports) this.#byTransport.set(transport, factory);
    }
  }

  get transports(): readonly DeviceTransport[] {
    return [...this.#byTransport.keys()];
  }

  open(device: MobileDevice, port: number, signal: AbortSignal): Promise<DeviceTunnel> {
    const factory = this.#byTransport.get(device.transport);
    if (factory === undefined) {
      return Promise.reject(new Error(`No tunnel factory for transport "${device.transport}"`));
    }
    return factory.open(device, port, signal);
  }
}

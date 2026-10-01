import type { Duplex } from 'node:stream';
import type { DeviceTransport, MobileDevice } from './DeviceWatcher.ts';

/**
 * A raw byte stream to the companion app on the device. The stream is delivered **paused** so no
 * byte is lost before the consumer is ready: attach listeners, then call `stream.resume()`.
 */
export interface DeviceTunnel extends AsyncDisposable {
  readonly stream: Duplex;
  /** Human-readable endpoint for logs, e.g. "usbmux:3:27100" or "adb:R5CT1234:27100". */
  readonly label: string;
}

export interface DeviceTunnelFactory {
  /** Transports this factory can open tunnels for. */
  readonly transports: readonly DeviceTransport[];
  /**
   * Opens a tunnel to `port` on `device`.
   * Rejects with `TransportError` codes APP_NOT_REACHABLE (nothing listening), DEVICE_UNAVAILABLE
   * or a service-unavailable code (USBMUX_UNAVAILABLE / ADB_UNAVAILABLE).
   */
  open(device: MobileDevice, port: number, signal: AbortSignal): Promise<DeviceTunnel>;
}

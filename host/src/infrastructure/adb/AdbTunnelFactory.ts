import type { DeviceTunnel, DeviceTunnelFactory } from '#ports/DeviceTunnelFactory.ts';
import type { DeviceTransport, MobileDevice } from '#ports/DeviceWatcher.ts';
import { socketTunnel } from '../tunnel/SocketTunnel.ts';
import type { AdbClient } from './AdbClient.ts';

/** Opens tunnels to the companion app on an Android phone through the ADB server (`host:transport` + `tcp:`). */
export class AdbTunnelFactory implements DeviceTunnelFactory {
  readonly transports: readonly DeviceTransport[] = ['adb'];
  readonly #client: AdbClient;

  constructor(client: AdbClient) {
    this.#client = client;
  }

  async open(device: MobileDevice, port: number, signal: AbortSignal): Promise<DeviceTunnel> {
    if (device.transport !== 'adb') throw new Error(`AdbTunnelFactory cannot reach a ${device.transport} device`);
    const { socket, leftover } = await this.#client.connect(device.serial, port, signal);
    return socketTunnel(socket, `adb:${device.serial}:${port}`, leftover);
  }
}

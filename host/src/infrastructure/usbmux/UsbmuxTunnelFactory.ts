import type { DeviceTunnel, DeviceTunnelFactory } from '#ports/DeviceTunnelFactory.ts';
import type { DeviceTransport, MobileDevice } from '#ports/DeviceWatcher.ts';
import { socketTunnel } from '../tunnel/SocketTunnel.ts';
import type { UsbmuxClient } from './UsbmuxClient.ts';

/** Opens tunnels to the companion app on an iPhone through usbmuxd `Connect`. */
export class UsbmuxTunnelFactory implements DeviceTunnelFactory {
  readonly transports: readonly DeviceTransport[] = ['usbmux'];
  readonly #client: UsbmuxClient;

  constructor(client: UsbmuxClient) {
    this.#client = client;
  }

  async open(device: MobileDevice, port: number, signal: AbortSignal): Promise<DeviceTunnel> {
    if (device.transport !== 'usbmux') throw new Error(`UsbmuxTunnelFactory cannot reach a ${device.transport} device`);
    const { socket, leftover } = await this.#client.connect(device.muxDeviceId, port, signal);
    return socketTunnel(socket, `usbmux:${device.muxDeviceId}:${port}`, leftover);
  }
}

import { connect } from 'node:net';
import { abortReason } from '#domain/abort.ts';
import { TransportError } from '#domain/errors.ts';
import type { DeviceTunnel, DeviceTunnelFactory } from '#ports/DeviceTunnelFactory.ts';
import type { DeviceTransport, MobileDevice } from '#ports/DeviceWatcher.ts';
import { socketTunnel } from '../tunnel/SocketTunnel.ts';

/**
 * Connects to the companion protocol over plain TCP instead of usbmux/ADB. Used by tests (the
 * fake companion) and for development against a device-side process reachable over the network.
 */
export class TcpTunnelFactory implements DeviceTunnelFactory {
  readonly transports: readonly DeviceTransport[] = ['tcp'];
  readonly #host: string;

  constructor(host = '127.0.0.1') {
    this.#host = host;
  }

  open(_device: MobileDevice, port: number, signal: AbortSignal): Promise<DeviceTunnel> {
    return new Promise((resolve, reject) => {
      const socket = connect({ host: this.#host, port });
      const onAbort = (): void => {
        socket.destroy();
        reject(abortReason(signal));
      };
      signal.addEventListener('abort', onAbort, { once: true });
      const onConnectError = (error: NodeJS.ErrnoException): void => {
        signal.removeEventListener('abort', onAbort);
        reject(
          error.code === 'ECONNREFUSED'
            ? new TransportError('APP_NOT_REACHABLE', `Nothing listening on ${this.#host}:${port}`, { cause: error })
            : new TransportError('CONNECTION_CLOSED', error.message, { cause: error }),
        );
      };
      socket.once('error', onConnectError);
      socket.once('connect', () => {
        signal.removeEventListener('abort', onAbort);
        socket.off('error', onConnectError);
        resolve(socketTunnel(socket, `tcp:${this.#host}:${port}`));
      });
    });
  }
}

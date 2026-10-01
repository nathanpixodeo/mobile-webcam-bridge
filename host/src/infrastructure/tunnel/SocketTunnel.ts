import type { Socket } from 'node:net';
import type { DeviceTunnel } from '#ports/DeviceTunnelFactory.ts';

/**
 * Wraps a connected, **paused** socket as a `DeviceTunnel`. Bytes that arrived together with the
 * multiplexer's final reply (usbmux Result, ADB OKAY) are pushed back so the protocol reader sees
 * the device stream from its first byte.
 */
export function socketTunnel(socket: Socket, label: string, leftover?: Buffer): DeviceTunnel {
  socket.pause();
  if (leftover !== undefined && leftover.length > 0) socket.unshift(leftover);
  socket.setNoDelay(true);
  // Errors after the handover reach the consumer as 'close'; never let one crash the process.
  socket.on('error', () => undefined);
  return {
    stream: socket,
    label,
    [Symbol.asyncDispose]: async () => {
      socket.destroy();
      await Promise.resolve();
    },
  };
}

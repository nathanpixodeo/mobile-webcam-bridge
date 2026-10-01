import { createServer, type Server, type Socket } from 'node:net';
import { build, type PlistValue } from 'plist';
import { ByteQueue } from '#domain/protocol/ByteQueue.ts';
import {
  decodeUsbmuxHeader,
  decodeUsbmuxPayload,
  USBMUX_HEADER_SIZE,
  type UsbmuxMessage,
} from '#infrastructure/usbmux/UsbmuxCodec.ts';

export type UsbmuxHandler = (
  request: UsbmuxMessage,
  reply: (message: Record<string, PlistValue>, trailing?: Buffer) => void,
  socket: Socket,
) => void;

/** Minimal usbmuxd speaking the plist framing, driven by a per-test request handler. */
export class FakeUsbmuxd implements AsyncDisposable {
  readonly #server: Server;
  readonly requests: UsbmuxMessage[] = [];

  private constructor(server: Server) {
    this.#server = server;
  }

  static async start(handler: UsbmuxHandler): Promise<FakeUsbmuxd> {
    const server = createServer();
    const fake = new FakeUsbmuxd(server);
    server.on('connection', (socket) => {
      const queue = new ByteQueue();
      socket.on('error', () => undefined);
      socket.on('data', (chunk: Buffer) => {
        queue.append(chunk);
        while (queue.length >= USBMUX_HEADER_SIZE) {
          const header = decodeUsbmuxHeader(queue.peek(USBMUX_HEADER_SIZE));
          if (queue.length < header.length) break;
          queue.skip(USBMUX_HEADER_SIZE);
          const request = decodeUsbmuxPayload(queue.take(header.length - USBMUX_HEADER_SIZE));
          fake.requests.push(request);
          handler(
            request,
            (message, trailing) => {
              socket.write(Buffer.concat([frame(header.tag, message), trailing ?? Buffer.alloc(0)]));
            },
            socket,
          );
        }
      });
    });
    await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
    return fake;
  }

  get address(): { host: string; port: number } {
    const address = this.#server.address();
    if (address === null || typeof address === 'string') throw new Error('not listening');
    return { host: '127.0.0.1', port: address.port };
  }

  async [Symbol.asyncDispose](): Promise<void> {
    await new Promise<void>((resolve) => {
      this.#server.close(() => {
        resolve();
      });
    });
  }
}

export function frame(tag: number, message: Record<string, PlistValue>): Buffer {
  const payload = Buffer.from(build(message), 'utf8');
  const header = Buffer.alloc(USBMUX_HEADER_SIZE);
  header.writeUInt32LE(USBMUX_HEADER_SIZE + payload.length, 0);
  header.writeUInt32LE(1, 4);
  header.writeUInt32LE(8, 8);
  header.writeUInt32LE(tag, 12);
  return Buffer.concat([header, payload]);
}

import { createServer, type Server, type Socket } from 'node:net';
import { ByteQueue } from '#domain/protocol/ByteQueue.ts';
import type { AdbAddress } from '#infrastructure/adb/AdbConnection.ts';

export interface AdbReply {
  okay(): void;
  fail(message: string): void;
  /** A length-prefixed text block: four hex digits of UTF-8 length, then the text. */
  block(text: string): void;
  /** Bytes written as they are, e.g. `OKAY` followed by tunnel data in one chunk. */
  raw(bytes: Buffer): void;
  /** Ends the connection, as a server does when it has nothing more to say. */
  close(): void;
}

export type AdbHandler = (service: string, reply: AdbReply, socket: Socket) => void;

/** Minimal ADB server speaking the smart-socket framing, driven by a per-test request handler. */
export class FakeAdbServer implements AsyncDisposable {
  readonly #server: Server;
  readonly #sockets = new Set<Socket>();
  /** Every service string received, in order, across all connections. */
  readonly requests: string[] = [];

  private constructor(server: Server) {
    this.#server = server;
  }

  static async start(handler: AdbHandler): Promise<FakeAdbServer> {
    const server = createServer();
    const fake = new FakeAdbServer(server);
    server.on('connection', (socket) => {
      fake.#sockets.add(socket);
      socket.on('close', () => {
        fake.#sockets.delete(socket);
      });
      socket.on('error', () => undefined);
      const queue = new ByteQueue();
      const reply = replyTo(socket);
      socket.on('data', (chunk: Buffer) => {
        queue.append(chunk);
        while (queue.length >= 4) {
          const prefix = queue.peek(4).toString('ascii');
          if (!/^[0-9a-f]{4}$/.test(prefix)) {
            socket.destroy();
            return;
          }
          const length = Number.parseInt(prefix, 16);
          if (queue.length < 4 + length) break;
          queue.skip(4);
          const service = queue.take(length).toString('utf8');
          fake.requests.push(service);
          handler(service, reply, socket);
        }
      });
    });
    await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
    return fake;
  }

  get address(): AdbAddress {
    const address = this.#server.address();
    if (address === null || typeof address === 'string') throw new Error('not listening');
    return { host: '127.0.0.1', port: address.port };
  }

  async [Symbol.asyncDispose](): Promise<void> {
    for (const socket of this.#sockets) socket.destroy();
    await new Promise<void>((resolve) => {
      this.#server.close(() => {
        resolve();
      });
    });
  }
}

function replyTo(socket: Socket): AdbReply {
  const lengthPrefixed = (text: string): Buffer => {
    const payload = Buffer.from(text, 'utf8');
    return Buffer.concat([Buffer.from(payload.length.toString(16).padStart(4, '0'), 'ascii'), payload]);
  };
  return {
    okay: () => {
      socket.write('OKAY');
    },
    fail: (message) => {
      socket.write(Buffer.concat([Buffer.from('FAIL'), lengthPrefixed(message)]));
    },
    block: (text) => {
      socket.write(lengthPrefixed(text));
    },
    raw: (bytes) => {
      socket.write(bytes);
    },
    close: () => {
      socket.end();
    },
  };
}

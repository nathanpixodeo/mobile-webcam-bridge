import { connect, type Socket } from 'node:net';
import { abortReason } from '#domain/abort.ts';
import { TransportError } from '#domain/errors.ts';
import { ByteQueue } from '#domain/protocol/ByteQueue.ts';
import { encodeAdbRequest, failureToError } from './AdbProtocol.ts';

export interface AdbAddress {
  readonly host: string;
  readonly port: number;
}

export interface AdbHandover {
  /** The socket, now a raw byte pipe to the device port. Ownership passes to the caller. */
  readonly socket: Socket;
  /** Stream bytes that arrived in the same chunk as the final OKAY. */
  readonly leftover: Buffer;
}

/** Builds the error for a socket that closes before a read is satisfied; the right code depends on what was read. */
type ClosedError = (cause: Error | undefined) => Error;

interface PendingRead {
  readonly count: number;
  readonly closedError: ClosedError;
  readonly resolve: (bytes: Buffer) => void;
  readonly reject: (error: Error) => void;
}

/**
 * One TCP connection to the ADB server, speaking the smart-socket protocol: each service request
 * is answered `OKAY` or `FAIL`. Reads are strictly sequential. After a `tcp:` request is accepted
 * the server stops framing, so `handover()` gives the raw socket (and buffered bytes) to the caller.
 */
export class AdbConnection implements AsyncDisposable {
  readonly #socket: Socket;
  readonly #queue = new ByteQueue();
  readonly #whenClosed = Promise.withResolvers<undefined>();
  #reader: PendingRead | undefined;
  #closeCause: Error | undefined;
  #closed = false;
  #handedOver = false;
  readonly #onData = (chunk: Buffer): void => {
    this.#queue.append(chunk);
    const reader = this.#reader;
    if (reader === undefined || this.#queue.length < reader.count) return;
    this.#reader = undefined;
    reader.resolve(this.#queue.take(reader.count));
  };

  private constructor(socket: Socket) {
    this.#socket = socket;
    socket.on('data', this.#onData);
    socket.on('error', (error) => {
      this.#close(error);
    });
    socket.on('close', () => {
      this.#close(undefined);
    });
  }

  static open(address: AdbAddress, signal?: AbortSignal): Promise<AdbConnection> {
    return new Promise((resolve, reject) => {
      if (signal?.aborted === true) {
        reject(abortReason(signal));
        return;
      }
      const socket = connect({ host: address.host, port: address.port });
      const onAbort = (): void => {
        socket.destroy();
        reject(abortReason(signal));
      };
      const cleanup = (): void => {
        socket.off('error', onError);
        signal?.removeEventListener('abort', onAbort);
      };
      const onError = (error: Error): void => {
        cleanup();
        reject(
          new TransportError(
            'ADB_UNAVAILABLE',
            `Cannot reach the ADB server at ${address.host}:${address.port}. Install Android platform-tools or run "adb start-server".`,
            { cause: error },
          ),
        );
      };
      socket.once('error', onError);
      socket.once('connect', () => {
        cleanup();
        socket.setNoDelay(true);
        resolve(new AdbConnection(socket));
      });
      signal?.addEventListener('abort', onAbort, { once: true });
    });
  }

  /** Resolves when the socket closes, however that happens. */
  get closed(): Promise<void> {
    return this.#whenClosed.promise;
  }

  /** Sends `service` and waits for the status. Resolves on `OKAY`; a `FAIL` rejects with the mapped error. */
  async request(service: string, signal?: AbortSignal): Promise<void> {
    this.#assertUsable();
    this.#socket.write(encodeAdbRequest(service));
    const closedError: ClosedError = (cause) => closedBeforeStatus(service, cause);
    const status = (await this.#read(4, closedError, signal)).toString('ascii');
    if (status === 'OKAY') return;
    if (status !== 'FAIL') {
      throw new TransportError('ADB_PROTOCOL', `ADB server answered "${service}" with unknown status "${status}"`);
    }
    throw failureToError(service, await this.#readText(closedError, signal));
  }

  /** Reads one length-prefixed text block (4 hex digits of UTF-8 length, then the text). */
  async readLengthPrefixed(signal?: AbortSignal): Promise<string> {
    this.#assertUsable();
    return this.#readText(
      (cause) => new TransportError('ADB_UNAVAILABLE', 'The ADB server closed the connection', { cause }),
      signal,
    );
  }

  /**
   * Turns the connection into a raw stream after a successful tunnel request. The socket is paused
   * and stops being parsed; bytes already buffered behind the final OKAY come back as `leftover`.
   */
  handover(): AdbHandover {
    this.#assertUsable();
    this.#handedOver = true;
    this.#socket.pause();
    this.#socket.off('data', this.#onData);
    return { socket: this.#socket, leftover: this.#queue.take(this.#queue.length) };
  }

  async [Symbol.asyncDispose](): Promise<void> {
    if (!this.#handedOver) this.#socket.destroy();
    await Promise.resolve();
  }

  #assertUsable(): void {
    if (this.#handedOver) throw new Error('ADB connection was handed over and can no longer be used');
  }

  async #readText(closedError: ClosedError, signal: AbortSignal | undefined): Promise<string> {
    const prefix = (await this.#read(4, closedError, signal)).toString('ascii');
    if (!/^[0-9a-f]{4}$/i.test(prefix)) {
      throw new TransportError('ADB_PROTOCOL', `ADB server sent an invalid length prefix "${prefix}"`);
    }
    const length = Number.parseInt(prefix, 16);
    if (length === 0) return '';
    return (await this.#read(length, closedError, signal)).toString('utf8');
  }

  #read(count: number, closedError: ClosedError, signal: AbortSignal | undefined): Promise<Buffer> {
    if (this.#reader !== undefined) return Promise.reject(new Error('ADB connection reads must be sequential'));
    // Buffered bytes win over a close, so a reply that arrived together with the FIN is not lost.
    if (this.#queue.length >= count) return Promise.resolve(this.#queue.take(count));
    if (this.#closed) return Promise.reject(closedError(this.#closeCause));
    if (signal?.aborted === true) return Promise.reject(abortReason(signal));
    return new Promise<Buffer>((resolve, reject) => {
      const onAbort = (): void => {
        this.#reader = undefined;
        reject(abortReason(signal));
      };
      this.#reader = {
        count,
        closedError,
        resolve: (bytes) => {
          signal?.removeEventListener('abort', onAbort);
          resolve(bytes);
        },
        reject: (error) => {
          signal?.removeEventListener('abort', onAbort);
          reject(error);
        },
      };
      signal?.addEventListener('abort', onAbort, { once: true });
    });
  }

  #close(cause: Error | undefined): void {
    if (this.#closed) return;
    this.#closed = true;
    this.#closeCause = cause;
    this.#whenClosed.resolve(undefined);
    const reader = this.#reader;
    this.#reader = undefined;
    reader?.reject(reader.closedError(cause));
  }
}

/**
 * A socket that closes before any status means nothing accepted the request. For `tcp:` that is
 * the app not listening: the server can answer that way instead of with `FAIL`.
 */
function closedBeforeStatus(service: string, cause: Error | undefined): TransportError {
  if (service.startsWith('tcp:')) {
    return new TransportError('APP_NOT_REACHABLE', `The ADB server closed the connection to the app (${service})`, {
      cause,
    });
  }
  return new TransportError('ADB_PROTOCOL', `The ADB server closed the connection before answering "${service}"`, {
    cause,
  });
}

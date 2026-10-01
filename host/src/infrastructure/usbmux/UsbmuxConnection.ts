import { connect, type Socket } from 'node:net';
import { abortReason } from '#domain/abort.ts';
import { TransportError } from '#domain/errors.ts';
import { Emitter, type EventSource } from '#domain/events.ts';
import { ByteQueue } from '#domain/protocol/ByteQueue.ts';
import {
  decodeUsbmuxHeader,
  decodeUsbmuxPayload,
  encodeUsbmuxMessage,
  resultCodeToError,
  toUsbmuxPortNumber,
  USBMUX_HEADER_SIZE,
  UsbmuxResultCode,
  type UsbmuxHeader,
  type UsbmuxMessage,
} from './UsbmuxCodec.ts';

export interface UsbmuxAddress {
  readonly host: string;
  readonly port: number;
}

export interface UsbmuxConnectionEvents {
  /** Unsolicited messages (Attached/Detached/Paired after `Listen`). */
  message: [message: UsbmuxMessage];
  closed: [error: Error | undefined];
}

interface PendingRequest {
  readonly resolve: (message: UsbmuxMessage) => void;
  readonly reject: (error: Error) => void;
  /** A successful answer turns the connection into a raw tunnel. */
  readonly opensTunnel: boolean;
}

export interface TunnelHandover {
  /** The socket, now a raw byte pipe to the device port. Ownership passes to the caller. */
  readonly socket: Socket;
  /** Tunnel bytes that arrived in the same chunk as the Connect result. */
  readonly leftover: Buffer;
}

/**
 * One TCP connection to usbmuxd. Requests are correlated by tag. `connectTunnel()` turns the
 * connection into a raw tunnel: after a successful `Connect` usbmuxd stops framing, so the
 * connection stops parsing and hands the socket (plus any already-buffered bytes) over.
 */
export class UsbmuxConnection implements AsyncDisposable {
  readonly #socket: Socket;
  readonly #queue = new ByteQueue();
  readonly #pending = new Map<number, PendingRequest>();
  readonly #events = new Emitter<UsbmuxConnectionEvents>();
  #header: UsbmuxHeader | undefined;
  #nextTag = 1;
  #handedOver = false;
  #closed = false;
  readonly #onData = (chunk: Buffer): void => {
    this.#receive(chunk);
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

  static open(address: UsbmuxAddress, signal?: AbortSignal): Promise<UsbmuxConnection> {
    return new Promise((resolve, reject) => {
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
            'USBMUX_UNAVAILABLE',
            `Cannot reach usbmuxd at ${address.host}:${address.port}. Is Apple Mobile Device Service running?`,
            { cause: error },
          ),
        );
      };
      socket.once('error', onError);
      socket.once('connect', () => {
        cleanup();
        socket.setNoDelay(true);
        resolve(new UsbmuxConnection(socket));
      });
      signal?.addEventListener('abort', onAbort, { once: true });
    });
  }

  get events(): EventSource<UsbmuxConnectionEvents> {
    return this.#events;
  }

  /** Sends `fields` and resolves with the response carrying the same tag. */
  request(fields: UsbmuxMessage, signal?: AbortSignal): Promise<UsbmuxMessage> {
    return this.#send(fields, false, signal);
  }

  /** Sends a request whose answer is a `Result`; rejects unless `Number` is 0. */
  async requestOk(fields: UsbmuxMessage, context: string, signal?: AbortSignal): Promise<void> {
    const response = await this.request(fields, signal);
    const code = resultNumber(response);
    if (code !== UsbmuxResultCode.Ok) throw resultCodeToError(code ?? -1, context);
  }

  /**
   * Asks usbmuxd to connect to `port` on the device. On success this connection is consumed:
   * the returned socket is paused and owned by the caller.
   */
  async connectTunnel(deviceId: number, port: number, signal?: AbortSignal): Promise<TunnelHandover> {
    const response = await this.#send(
      { MessageType: 'Connect', DeviceID: deviceId, PortNumber: toUsbmuxPortNumber(port) },
      true,
      signal,
    );
    const code = resultNumber(response);
    if (code !== UsbmuxResultCode.Ok) {
      this.#socket.destroy();
      throw resultCodeToError(code ?? -1, `Connect to port ${port}`);
    }
    // #receive already paused the socket and stopped parsing when it saw this response.
    return { socket: this.#socket, leftover: this.#queue.take(this.#queue.length) };
  }

  async [Symbol.asyncDispose](): Promise<void> {
    if (!this.#handedOver) this.#socket.destroy();
    await Promise.resolve();
  }

  #send(fields: UsbmuxMessage, opensTunnel: boolean, signal?: AbortSignal): Promise<UsbmuxMessage> {
    if (this.#closed || this.#handedOver) {
      return Promise.reject(new TransportError('USBMUX_UNAVAILABLE', 'usbmux connection is closed'));
    }
    const tag = this.#nextTag++;
    return new Promise<UsbmuxMessage>((resolve, reject) => {
      const onAbort = (): void => {
        this.#pending.delete(tag);
        reject(abortReason(signal));
      };
      this.#pending.set(tag, {
        opensTunnel,
        resolve: (message) => {
          signal?.removeEventListener('abort', onAbort);
          resolve(message);
        },
        reject: (error) => {
          signal?.removeEventListener('abort', onAbort);
          reject(error);
        },
      });
      signal?.addEventListener('abort', onAbort, { once: true });
      this.#socket.write(encodeUsbmuxMessage(tag, fields));
    });
  }

  #receive(chunk: Buffer): void {
    this.#queue.append(chunk);
    try {
      while (!this.#handedOver) {
        if (this.#header === undefined) {
          if (this.#queue.length < USBMUX_HEADER_SIZE) return;
          this.#header = decodeUsbmuxHeader(this.#queue.take(USBMUX_HEADER_SIZE));
        }
        const payloadLength = this.#header.length - USBMUX_HEADER_SIZE;
        if (this.#queue.length < payloadLength) return;
        const tag = this.#header.tag;
        this.#header = undefined;
        const message = decodeUsbmuxPayload(this.#queue.take(payloadLength));
        this.#dispatch(tag, message);
      }
    } catch (error) {
      this.#socket.destroy(error instanceof Error ? error : new Error(String(error)));
    }
  }

  #dispatch(tag: number, message: UsbmuxMessage): void {
    const pending = tag === 0 ? undefined : this.#pending.get(tag);
    if (pending === undefined) {
      this.#events.emit('message', message);
      return;
    }
    this.#pending.delete(tag);
    if (pending.opensTunnel && resultNumber(message) === UsbmuxResultCode.Ok) {
      // Successful Connect: from now on the socket carries raw tunnel bytes.
      this.#handedOver = true;
      this.#socket.pause();
      this.#socket.off('data', this.#onData);
    }
    pending.resolve(message);
  }

  #close(error: Error | undefined): void {
    if (this.#closed || this.#handedOver) return;
    this.#closed = true;
    const failure = new TransportError('USBMUX_UNAVAILABLE', 'usbmux connection closed', { cause: error });
    for (const pending of this.#pending.values()) pending.reject(failure);
    this.#pending.clear();
    this.#events.emit('closed', error);
  }
}

function resultNumber(message: UsbmuxMessage): number | undefined {
  const value = message['Number'];
  return typeof value === 'number' ? value : undefined;
}

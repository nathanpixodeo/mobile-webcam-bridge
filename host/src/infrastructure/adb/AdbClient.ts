import { TransportError } from '#domain/errors.ts';
import { AdbConnection, type AdbAddress, type AdbHandover } from './AdbConnection.ts';
import { parseDeviceList, type AdbDeviceEntry } from './AdbProtocol.ts';

/**
 * High-level ADB server operations. The server dedicates a connection to each service (a
 * `host:transport` or `host:track-devices` request consumes it), so every call opens a fresh
 * `AdbConnection`.
 */
export class AdbClient {
  readonly #address: AdbAddress;

  constructor(address: AdbAddress) {
    this.#address = address;
  }

  get address(): AdbAddress {
    return this.#address;
  }

  /** The server's protocol version (41 for current platform-tools). */
  async version(signal?: AbortSignal): Promise<number> {
    await using connection = await AdbConnection.open(this.#address, signal);
    await connection.request('host:version', signal);
    const text = await connection.readLengthPrefixed(signal);
    if (!/^[0-9a-f]+$/i.test(text)) {
      throw new TransportError('ADB_PROTOCOL', `ADB server returned an invalid version "${text}"`);
    }
    return Number.parseInt(text, 16);
  }

  async listDevices(signal?: AbortSignal): Promise<AdbDeviceEntry[]> {
    await using connection = await AdbConnection.open(this.#address, signal);
    await connection.request('host:devices-l', signal);
    return parseDeviceList(await connection.readLengthPrefixed(signal));
  }

  /**
   * Opens a connection subscribed to device changes: read the current list and every later change
   * with `readLengthPrefixed()`. Dispose the connection to stop tracking.
   */
  async trackDevices(signal?: AbortSignal): Promise<AdbConnection> {
    try {
      return await this.#subscribe('host:track-devices-l', signal);
    } catch (error) {
      if (!(error instanceof TransportError) || error.code !== 'ADB_PROTOCOL') throw error;
      // Servers that predate the long format reject `-l`; the short format carries what the watcher needs.
      return this.#subscribe('host:track-devices', signal);
    }
  }

  /**
   * Opens a stream to `port` on the phone's loopback interface. On success the returned socket is
   * paused and owned by the caller. Rejects with DEVICE_UNAVAILABLE for an unauthorized, offline or
   * unknown phone and with APP_NOT_REACHABLE when nothing listens on `port`.
   */
  async connect(serial: string, port: number, signal?: AbortSignal): Promise<AdbHandover> {
    const connection = await AdbConnection.open(this.#address, signal);
    try {
      await connection.request(`host:transport:${serial}`, signal);
      await connection.request(`tcp:${port}`, signal);
      return connection.handover();
    } catch (error) {
      await connection[Symbol.asyncDispose]();
      throw error;
    }
  }

  async #subscribe(service: string, signal: AbortSignal | undefined): Promise<AdbConnection> {
    const connection = await AdbConnection.open(this.#address, signal);
    try {
      await connection.request(service, signal);
      return connection;
    } catch (error) {
      await connection[Symbol.asyncDispose]();
      throw error;
    }
  }
}

import type { PlistValue } from 'plist';
import { TransportError } from '#domain/errors.ts';
import type { DeviceLink, UsbmuxDevice } from '#ports/DeviceWatcher.ts';
import { UsbmuxConnection, type TunnelHandover, type UsbmuxAddress } from './UsbmuxConnection.ts';
import { UsbmuxResultCode, type UsbmuxMessage } from './UsbmuxCodec.ts';

/**
 * High-level usbmuxd operations. usbmuxd expects one connection per operation (a `Connect` or a
 * `Listen` consumes the connection), so every call opens a fresh `UsbmuxConnection`.
 */
export class UsbmuxClient {
  readonly #address: UsbmuxAddress;

  constructor(address: UsbmuxAddress) {
    this.#address = address;
  }

  get address(): UsbmuxAddress {
    return this.#address;
  }

  async listDevices(signal?: AbortSignal): Promise<UsbmuxDevice[]> {
    await using connection = await UsbmuxConnection.open(this.#address, signal);
    const response = await connection.request({ MessageType: 'ListDevices' }, signal);
    const list = response['DeviceList'];
    if (!Array.isArray(list)) throw new TransportError('USBMUX_PROTOCOL', 'ListDevices returned no DeviceList');
    return list.flatMap((entry) => {
      const device = parseDeviceEntry(entry);
      return device === undefined ? [] : [device];
    });
  }

  /**
   * True when Windows holds a pairing record for the device (the user tapped "Trust").
   * Undefined when this usbmuxd does not support the query.
   */
  async isPaired(udid: string, signal?: AbortSignal): Promise<boolean | undefined> {
    await using connection = await UsbmuxConnection.open(this.#address, signal);
    const response = await connection.request({ MessageType: 'ReadPairRecord', PairRecordID: udid }, signal);
    if (response['PairRecordData'] !== undefined) return true;
    const code = response['Number'];
    if (code === UsbmuxResultCode.BadCommand) return undefined;
    return false;
  }

  async connect(muxDeviceId: number, port: number, signal?: AbortSignal): Promise<TunnelHandover> {
    const connection = await UsbmuxConnection.open(this.#address, signal);
    return connection.connectTunnel(muxDeviceId, port, signal);
  }

  /** Opens a connection subscribed to Attached/Detached events. Dispose it to stop listening. */
  async listen(signal?: AbortSignal): Promise<UsbmuxConnection> {
    const connection = await UsbmuxConnection.open(this.#address, signal);
    try {
      await connection.requestOk({ MessageType: 'Listen' }, 'Listen', signal);
      return connection;
    } catch (error) {
      await connection[Symbol.asyncDispose]();
      throw error;
    }
  }
}

/** Parses an `Attached` message or a `DeviceList` entry. */
export function parseDeviceEntry(entry: PlistValue | UsbmuxMessage): UsbmuxDevice | undefined {
  if (!isDictionary(entry)) return undefined;
  const properties = entry['Properties'];
  if (!isDictionary(properties)) return undefined;
  const muxDeviceId = properties['DeviceID'] ?? entry['DeviceID'];
  const udid = properties['SerialNumber'];
  const connectionType = properties['ConnectionType'];
  const productId = properties['ProductID'];
  if (typeof muxDeviceId !== 'number' || typeof udid !== 'string') return undefined;
  return {
    transport: 'usbmux',
    platform: 'ios',
    id: udid,
    muxDeviceId,
    link: toLink(connectionType),
    model: undefined,
    productId: typeof productId === 'number' ? productId : undefined,
  };
}

function toLink(connectionType: PlistValue | undefined): DeviceLink {
  return connectionType === 'Network' ? 'network' : 'usb';
}

function isDictionary(value: PlistValue | UsbmuxMessage | undefined): value is Readonly<Record<string, PlistValue>> {
  return (
    value !== null &&
    value !== undefined &&
    typeof value === 'object' &&
    !Array.isArray(value) &&
    !(value instanceof Date) &&
    !(value instanceof Uint8Array)
  );
}

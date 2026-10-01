/**
 * usbmuxd framing (as spoken by Apple Mobile Device Service on 127.0.0.1:27015): a 16-byte
 * little-endian header `{length incl. header, version, message, tag}` followed by an XML plist.
 * Reference: libimobiledevice/libusbmuxd.
 */
import { build, parse, type PlistValue } from 'plist';
import { TransportError } from '#domain/errors.ts';

export const USBMUX_HEADER_SIZE = 16;
export const USBMUX_VERSION_PLIST = 1;
export const USBMUX_MESSAGE_PLIST = 8;
/** usbmuxd rejects anything larger; it also bounds what we buffer. */
export const USBMUX_MAX_PAYLOAD = 1024 * 1024;

export type UsbmuxMessage = Readonly<Record<string, PlistValue>>;

export interface UsbmuxHeader {
  readonly length: number;
  readonly version: number;
  readonly message: number;
  readonly tag: number;
}

const CLIENT_FIELDS = {
  ClientVersionString: 'mobile-webcam-bridge-host',
  ProgName: 'mobile-webcam-bridge',
  kLibUSBMuxVersion: 3,
} as const;

export function encodeUsbmuxMessage(tag: number, fields: UsbmuxMessage): Buffer {
  const payload = Buffer.from(build({ ...fields, ...CLIENT_FIELDS }), 'utf8');
  const header = Buffer.allocUnsafe(USBMUX_HEADER_SIZE);
  header.writeUInt32LE(USBMUX_HEADER_SIZE + payload.length, 0);
  header.writeUInt32LE(USBMUX_VERSION_PLIST, 4);
  header.writeUInt32LE(USBMUX_MESSAGE_PLIST, 8);
  header.writeUInt32LE(tag, 12);
  return Buffer.concat([header, payload]);
}

export function decodeUsbmuxHeader(bytes: Buffer): UsbmuxHeader {
  const header: UsbmuxHeader = {
    length: bytes.readUInt32LE(0),
    version: bytes.readUInt32LE(4),
    message: bytes.readUInt32LE(8),
    tag: bytes.readUInt32LE(12),
  };
  const payloadLength = header.length - USBMUX_HEADER_SIZE;
  if (payloadLength < 0 || payloadLength > USBMUX_MAX_PAYLOAD) {
    throw new TransportError('USBMUX_PROTOCOL', `Invalid usbmux packet length ${header.length}`);
  }
  if (header.version !== USBMUX_VERSION_PLIST || header.message !== USBMUX_MESSAGE_PLIST) {
    throw new TransportError(
      'USBMUX_BAD_VERSION',
      `Unsupported usbmux framing (version ${header.version}, message ${header.message})`,
    );
  }
  return header;
}

export function decodeUsbmuxPayload(payload: Buffer): UsbmuxMessage {
  let value: PlistValue;
  try {
    value = parse(payload.toString('utf8'));
  } catch (error) {
    throw new TransportError('USBMUX_PROTOCOL', 'usbmux payload is not a valid plist', { cause: error });
  }
  if (
    value === null ||
    typeof value !== 'object' ||
    Array.isArray(value) ||
    value instanceof Date ||
    value instanceof Uint8Array
  ) {
    throw new TransportError('USBMUX_PROTOCOL', 'usbmux payload is not a dictionary');
  }
  return value;
}

/** usbmuxd expects the TCP port in network byte order inside a host-order integer field. */
export function toUsbmuxPortNumber(port: number): number {
  if (!Number.isInteger(port) || port <= 0 || port > 0xffff) throw new RangeError(`Invalid port ${port}`);
  return ((port & 0xff) << 8) | (port >> 8);
}

export const UsbmuxResultCode = {
  Ok: 0,
  BadCommand: 1,
  BadDevice: 2,
  ConnectionRefused: 3,
  BadVersion: 6,
} as const;

export function resultCodeToError(code: number, context: string): TransportError {
  switch (code) {
    case UsbmuxResultCode.BadDevice:
      return new TransportError('DEVICE_UNAVAILABLE', `${context}: device is no longer connected`);
    case UsbmuxResultCode.ConnectionRefused:
      return new TransportError('APP_NOT_REACHABLE', `${context}: nothing is listening on the device port`);
    case UsbmuxResultCode.BadVersion:
      return new TransportError('USBMUX_BAD_VERSION', `${context}: usbmuxd rejected the protocol version`);
    default:
      return new TransportError('USBMUX_PROTOCOL', `${context}: usbmuxd returned error ${code}`);
  }
}

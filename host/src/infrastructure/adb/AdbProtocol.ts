import { TransportError } from '#domain/errors.ts';
import type { AdbDevice, DeviceLink, PendingDevice } from '#ports/DeviceWatcher.ts';

/** The request length prefix is four hex digits, which caps a service string at 65 535 bytes. */
const MAX_SERVICE_BYTES = 0xffff;

/** One line of `host:devices-l` / `host:track-devices[-l]` output. */
export interface AdbDeviceEntry {
  readonly serial: string;
  /** `device`, `unauthorized`, `offline`, ... */
  readonly state: string;
  /** `key:value` fields of the long format (`model`, `product`, `transport_id`, ...); empty in the short format. */
  readonly properties: ReadonlyMap<string, string>;
}

/** Frames a smart-socket request: the service's UTF-8 byte length as four hex digits, then the service. */
export function encodeAdbRequest(service: string): Buffer {
  const payload = Buffer.from(service, 'utf8');
  if (payload.length > MAX_SERVICE_BYTES) {
    throw new RangeError(`ADB service string is ${payload.length} bytes; the limit is ${MAX_SERVICE_BYTES}`);
  }
  return Buffer.concat([Buffer.from(payload.length.toString(16).padStart(4, '0'), 'ascii'), payload]);
}

/**
 * Parses a device list in either format: `SERIAL\tSTATE` (`track-devices`) or
 * `SERIAL<spaces>STATE usb:1-1 product:x model:Pixel_9 ...` (`-l`).
 */
export function parseDeviceList(text: string): AdbDeviceEntry[] {
  const entries: AdbDeviceEntry[] = [];
  for (const line of text.split('\n')) {
    const [serial, state, ...fields] = line.trim().split(/\s+/);
    // A blank line splits into one empty token.
    if (serial === undefined || serial === '' || state === undefined) continue;
    const properties = new Map<string, string>();
    for (const field of fields) {
      const separator = field.indexOf(':');
      if (separator > 0) properties.set(field.slice(0, separator), field.slice(separator + 1));
    }
    entries.push({ serial, state, properties });
  }
  return entries;
}

/** Wireless debugging serials are `host:port` or an mDNS name containing `._tcp`; USB serials are neither. */
export function adbLink(serial: string): DeviceLink {
  return serial.includes(':') || serial.includes('._tcp') ? 'network' : 'usb';
}

/** A phone that is ready to use (`device` state); undefined for every other state. */
export function toAdbDevice(entry: AdbDeviceEntry): AdbDevice | undefined {
  if (entry.state !== 'device') return undefined;
  return {
    transport: 'adb',
    platform: 'android',
    id: entry.serial,
    serial: entry.serial,
    link: adbLink(entry.serial),
    model: entry.properties.get('model')?.replaceAll('_', ' '),
  };
}

/** A connected phone that needs action first (`unauthorized`, `offline`, ...); undefined when it is ready. */
export function toPendingDevice(entry: AdbDeviceEntry): PendingDevice | undefined {
  if (entry.state === 'device') return undefined;
  return { id: entry.serial, platform: 'android', state: entry.state };
}

/** Maps the message of a `FAIL` reply to the error the rest of the host understands. */
export function failureToError(service: string, message: string): TransportError {
  const reason = message.toLowerCase();
  if (reason.includes('unauthorized')) {
    return new TransportError(
      'DEVICE_UNAVAILABLE',
      `Android device is unauthorized: unlock it and accept the "Allow USB debugging" prompt (${message})`,
    );
  }
  if (reason.includes('offline') || reason.includes('not found') || reason.includes('no devices')) {
    return new TransportError('DEVICE_UNAVAILABLE', `Android device is not available over ADB (${message})`);
  }
  // The only way `tcp:<port>` fails after the transport was selected is that nothing accepted on the phone.
  if (service.startsWith('tcp:')) {
    return new TransportError('APP_NOT_REACHABLE', `The Android app is not reachable (${service}: ${message})`);
  }
  return new TransportError('ADB_PROTOCOL', `ADB server rejected "${service}" (${message})`);
}

import type { BridgeError } from '#domain/errors.ts';
import type { EventSource } from '#domain/events.ts';

export type DevicePlatform = 'ios' | 'android' | 'unknown';

/** Physical link to the phone: a USB cable, or a network transport (Wi-Fi sync, wireless debugging). */
export type DeviceLink = 'usb' | 'network';

interface DeviceBase {
  /** Stable identity across reconnects: the iPhone UDID or the Android serial number. */
  readonly id: string;
  readonly platform: DevicePlatform;
  readonly link: DeviceLink;
  /** Model as reported by the transport, when it reports one. */
  readonly model: string | undefined;
}

/** An iPhone reached through usbmuxd (Apple Mobile Device Service). */
export interface UsbmuxDevice extends DeviceBase {
  readonly transport: 'usbmux';
  readonly platform: 'ios';
  /** usbmuxd's transient device id, needed for `Connect`. Changes on every re-attach. */
  readonly muxDeviceId: number;
  readonly productId: number | undefined;
}

/** An Android phone reached through the ADB server. */
export interface AdbDevice extends DeviceBase {
  readonly transport: 'adb';
  readonly platform: 'android';
  /** ADB serial (`host:transport:<serial>`). */
  readonly serial: string;
}

/** A device-side process reached over plain TCP (development and tests). */
export interface TcpDevice extends DeviceBase {
  readonly transport: 'tcp';
}

export type MobileDevice = UsbmuxDevice | AdbDevice | TcpDevice;
export type DeviceTransport = MobileDevice['transport'];

/** Two device records describe the same live attachment (a re-attach changes the usbmux id). */
export function sameAttachment(a: MobileDevice, b: MobileDevice): boolean {
  if (a.transport !== b.transport || a.id !== b.id) return false;
  return a.transport === 'usbmux' && b.transport === 'usbmux' ? a.muxDeviceId === b.muxDeviceId : true;
}

export function describeDevice(device: MobileDevice): string {
  const platform = device.platform === 'ios' ? 'iPhone' : device.platform === 'android' ? 'Android' : 'device';
  return `${platform} ${device.model ?? device.id}`;
}

export interface DeviceWatcherEvents {
  attached: [device: MobileDevice];
  detached: [id: string];
  /** The watcher lost its device service (it keeps retrying); its devices are reported detached. */
  unavailable: [error: BridgeError];
}

/**
 * A device that is connected but not usable yet (Android: "unauthorized" until the user accepts
 * the USB debugging prompt; "offline" while connecting).
 */
export interface PendingDevice {
  readonly id: string;
  readonly platform: DevicePlatform;
  readonly state: string;
}

export interface DeviceWatcher extends AsyncDisposable {
  readonly events: EventSource<DeviceWatcherEvents>;
  /** Starts watching; resolves once the initial device list is known (or the service is unavailable). */
  start(): Promise<void>;
  /** Currently attached, usable devices. */
  devices(): readonly MobileDevice[];
  /** Connected devices that need user action before they can be used. */
  pending(): readonly PendingDevice[];
}

/**
 * Error hierarchy of the host. Every failure that crosses a module boundary is a `BridgeError`
 * with a machine-readable `code` (a string-literal union, so `switch` statements are exhaustive)
 * and a `retryable` hint used by supervisors.
 */

interface BridgeErrorOptions {
  readonly cause?: unknown;
  readonly retryable?: boolean;
}

export abstract class BridgeError extends Error {
  abstract readonly code: BridgeErrorCode;
  readonly retryable: boolean;

  protected constructor(message: string, options: BridgeErrorOptions = {}) {
    super(message, options.cause === undefined ? undefined : { cause: options.cause });
    this.name = new.target.name;
    this.retryable = options.retryable ?? false;
  }
}

export type ConfigErrorCode = 'CONFIG_INVALID';

export class ConfigError extends BridgeError {
  readonly code: ConfigErrorCode = 'CONFIG_INVALID';

  constructor(message: string, options: BridgeErrorOptions = {}) {
    super(message, options);
  }
}

export type TransportErrorCode =
  | 'USBMUX_UNAVAILABLE'
  | 'USBMUX_BAD_VERSION'
  | 'USBMUX_PROTOCOL'
  | 'ADB_UNAVAILABLE'
  | 'ADB_PROTOCOL'
  | 'DEVICE_UNAVAILABLE'
  | 'APP_NOT_REACHABLE'
  | 'CONNECTION_CLOSED'
  | 'HANDSHAKE_TIMEOUT'
  | 'HEARTBEAT_TIMEOUT';

const RETRYABLE_TRANSPORT_CODES: ReadonlySet<TransportErrorCode> = new Set([
  'USBMUX_UNAVAILABLE',
  'ADB_UNAVAILABLE',
  'APP_NOT_REACHABLE',
  'CONNECTION_CLOSED',
  'HANDSHAKE_TIMEOUT',
  'HEARTBEAT_TIMEOUT',
]);

export class TransportError extends BridgeError {
  readonly code: TransportErrorCode;

  constructor(code: TransportErrorCode, message: string, options: { readonly cause?: unknown } = {}) {
    super(message, { ...options, retryable: RETRYABLE_TRANSPORT_CODES.has(code) });
    this.code = code;
  }
}

/** Low-level framing violations detected by the packet reader. */
export type ProtocolViolation = 'BAD_MAGIC' | 'PAYLOAD_TOO_LARGE' | 'BAD_PAYLOAD_LENGTH' | 'BAD_MESSAGE';

export type ProtocolErrorCode = 'PROTOCOL_VIOLATION' | 'VERSION_MISMATCH';

export class ProtocolError extends BridgeError {
  readonly code: ProtocolErrorCode;
  readonly violation: ProtocolViolation | undefined;

  private constructor(code: ProtocolErrorCode, message: string, violation?: ProtocolViolation, cause?: unknown) {
    super(message, { cause, retryable: code === 'PROTOCOL_VIOLATION' });
    this.code = code;
    this.violation = violation;
  }

  static violation(violation: ProtocolViolation, message: string, cause?: unknown): ProtocolError {
    return new ProtocolError('PROTOCOL_VIOLATION', message, violation, cause);
  }

  static versionMismatch(localMajor: number, remoteMajor: number): ProtocolError {
    return new ProtocolError(
      'VERSION_MISMATCH',
      `Protocol major version mismatch: host ${localMajor}, device ${remoteMajor}. Update the older side.`,
    );
  }
}

export type MediaErrorCode =
  | 'DECODER_FAILED'
  | 'FFMPEG_NOT_FOUND'
  | 'NATIVE_HELPER_FAILED'
  | 'NATIVE_NOT_INSTALLED'
  | 'HUB_IN_USE'
  | 'AUDIO_SINK_FAILED';

export class MediaError extends BridgeError {
  readonly code: MediaErrorCode;

  constructor(code: MediaErrorCode, message: string, options: BridgeErrorOptions = {}) {
    super(message, options);
    this.code = code;
  }
}

export type InstallErrorCode = 'ELEVATION_CANCELLED' | 'INSTALL_FAILED' | 'NOT_SUPPORTED_OS';

export class InstallError extends BridgeError {
  readonly code: InstallErrorCode;

  constructor(code: InstallErrorCode, message: string, options: BridgeErrorOptions = {}) {
    super(message, options);
    this.code = code;
  }
}

/** An `Error` packet received from the device. */
export class RemoteError extends BridgeError {
  readonly code = 'REMOTE_ERROR' as const;
  readonly remoteCode: string;
  readonly fatal: boolean;

  constructor(remoteCode: string, message: string, fatal: boolean) {
    super(`Device reported ${remoteCode}: ${message}`, { retryable: !fatal });
    this.remoteCode = remoteCode;
    this.fatal = fatal;
  }
}

export class DisposedError extends BridgeError {
  readonly code = 'DISPOSED' as const;

  constructor(what: string) {
    super(`${what} has been disposed`);
  }
}

export type BridgeErrorCode =
  | ConfigErrorCode
  | TransportErrorCode
  | ProtocolErrorCode
  | MediaErrorCode
  | InstallErrorCode
  | 'REMOTE_ERROR'
  | 'DISPOSED';

export function isBridgeError(error: unknown): error is BridgeError {
  return error instanceof BridgeError;
}

/** Normalises anything thrown into an `Error` for logging. */
export function toError(value: unknown): Error {
  return value instanceof Error ? value : new Error(String(value));
}

export function isAbortError(error: unknown): boolean {
  return error instanceof Error && error.name === 'AbortError';
}

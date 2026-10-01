import type { BridgeError } from '#domain/errors.ts';
import { ExponentialBackoff, type BackoffOptions } from './ExponentialBackoff.ts';

export type ReconnectDecision =
  | { readonly kind: 'retry'; readonly delayMs: number; readonly quiet: boolean }
  | { readonly kind: 'waitForReattach' }
  | { readonly kind: 'giveUp' };

export interface ReconnectPolicyOptions {
  readonly backoff: BackoffOptions;
  /** Poll interval while the companion app is not listening (usbmux ConnectionRefused, ADB closed stream). */
  readonly appPollMs: number;
  /** A connection that stayed up at least this long resets the backoff. */
  readonly stableAfterMs: number;
}

/**
 * Decides what a device session does after a failure. Pure policy: no timers, no I/O.
 *
 * - App not listening yet: poll at a fixed, short interval (the user is about to open it).
 * - Device gone: stop and wait for the watcher to report it again.
 * - Version mismatch: give up until the device re-attaches (retrying cannot help).
 * - Everything else retryable: exponential backoff, reset once a connection proved stable.
 */
export class ReconnectPolicy {
  readonly #options: ReconnectPolicyOptions;
  readonly #backoff: ExponentialBackoff;
  #appNotReachableStreak = 0;

  constructor(options: ReconnectPolicyOptions) {
    this.#options = options;
    this.#backoff = new ExponentialBackoff(options.backoff);
  }

  onConnected(): void {
    this.#appNotReachableStreak = 0;
  }

  onDisconnected(connectedForMs: number): void {
    if (connectedForMs >= this.#options.stableAfterMs) this.#backoff.reset();
  }

  decide(error: BridgeError): ReconnectDecision {
    switch (error.code) {
      case 'APP_NOT_REACHABLE':
        this.#appNotReachableStreak++;
        return { kind: 'retry', delayMs: this.#options.appPollMs, quiet: this.#appNotReachableStreak > 1 };
      case 'DEVICE_UNAVAILABLE':
        return { kind: 'waitForReattach' };
      case 'VERSION_MISMATCH':
        return { kind: 'giveUp' };
      default:
        this.#appNotReachableStreak = 0;
        // Device-reported errors are retried too: the user may fix the cause (permissions,
        // camera in use) on the phone while we back off.
        return error.retryable || error.code === 'REMOTE_ERROR'
          ? { kind: 'retry', delayMs: this.#backoff.next(), quiet: false }
          : { kind: 'giveUp' };
    }
  }
}

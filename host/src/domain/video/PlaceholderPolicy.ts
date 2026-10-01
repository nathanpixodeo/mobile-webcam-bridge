import type { StreamState } from '#domain/protocol/messages.ts';

/** Placeholder images the video hub can show instead of live video. */
export const PLACEHOLDER_KINDS = ['no-device', 'app-closed', 'paused', 'background', 'stopped'] as const;
export type PlaceholderKind = (typeof PLACEHOLDER_KINDS)[number];

export interface PlaceholderInputs {
  readonly deviceAttached: boolean;
  /** Handshake with the companion app completed and the connection is alive. */
  readonly sessionReady: boolean;
  /** Last video state reported by the device, if any. */
  readonly videoState: StreamState | undefined;
  readonly videoReason: string | undefined;
}

/**
 * Chooses what the virtual camera should show. `null` means "live": the hub shows decoded
 * frames and falls back to its `stopped` placeholder on its own when frames stop arriving.
 */
export function selectPlaceholder(inputs: PlaceholderInputs): PlaceholderKind | null {
  if (!inputs.deviceAttached) return 'no-device';
  if (!inputs.sessionReady) return 'app-closed';
  switch (inputs.videoState) {
    case 'interrupted':
      return inputs.videoReason === 'background' ? 'background' : 'paused';
    case 'error':
      return 'paused';
    case 'off':
    case 'starting':
    case 'running':
    case undefined:
      return null;
  }
}

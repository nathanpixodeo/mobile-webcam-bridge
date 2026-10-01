import type { StartAudio, StartVideo } from '#domain/protocol/messages.ts';

export interface DesiredStreams {
  readonly video: StartVideo | null;
  readonly audio: StartAudio | null;
}

export const NO_STREAMS: DesiredStreams = { video: null, audio: null };

export type StreamCommand =
  | { readonly kind: 'startVideo'; readonly params: StartVideo }
  | { readonly kind: 'stopVideo' }
  | { readonly kind: 'startAudio'; readonly params: StartAudio }
  | { readonly kind: 'stopAudio' };

/**
 * Level-triggered reconciliation of the streams the host wants against what it last asked the
 * device for. Calling `reconcile` repeatedly is idempotent; `reset` (new connection) forgets
 * what was requested so every desired stream is requested again.
 */
export class StreamReconciler {
  #requested: DesiredStreams = NO_STREAMS;

  reset(): void {
    this.#requested = NO_STREAMS;
  }

  reconcile(desired: DesiredStreams): StreamCommand[] {
    const commands: StreamCommand[] = [];
    if (desired.video === null) {
      if (this.#requested.video !== null) commands.push({ kind: 'stopVideo' });
    } else if (!sameParams(desired.video, this.#requested.video)) {
      commands.push({ kind: 'startVideo', params: desired.video });
    }
    if (desired.audio === null) {
      if (this.#requested.audio !== null) commands.push({ kind: 'stopAudio' });
    } else if (!sameParams(desired.audio, this.#requested.audio)) {
      commands.push({ kind: 'startAudio', params: desired.audio });
    }
    this.#requested = desired;
    return commands;
  }
}

function sameParams<T extends object>(a: T, b: T | null): boolean {
  if (b === null) return false;
  const keys = Object.keys(a) as (keyof T)[];
  return keys.length === Object.keys(b).length && keys.every((key) => a[key] === b[key]);
}

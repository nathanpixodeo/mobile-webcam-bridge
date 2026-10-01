import type { StartAudio, StartVideo } from '#domain/protocol/messages.ts';
import type { DesiredStreams } from '#domain/session/StreamReconciler.ts';

export interface StreamCoordinatorOptions {
  readonly video: StartVideo;
  /** Null when the virtual microphone is unavailable. */
  readonly audio: StartAudio | null;
  readonly onChange: (desired: DesiredStreams) => void;
}

/** Maps the two demand signals (camera consumers, microphone capture) to desired device streams. */
export class StreamCoordinator {
  readonly #options: StreamCoordinatorOptions;
  #videoDemand = false;
  #audioDemand = false;

  constructor(options: StreamCoordinatorOptions) {
    this.#options = options;
  }

  get desired(): DesiredStreams {
    return {
      video: this.#videoDemand ? this.#options.video : null,
      audio: this.#audioDemand ? this.#options.audio : null,
    };
  }

  setVideoDemand(active: boolean): void {
    if (active === this.#videoDemand) return;
    this.#videoDemand = active;
    this.#options.onChange(this.desired);
  }

  setAudioDemand(active: boolean): void {
    if (active === this.#audioDemand) return;
    this.#audioDemand = active;
    this.#options.onChange(this.desired);
  }
}

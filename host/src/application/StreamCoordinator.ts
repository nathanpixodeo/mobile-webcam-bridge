import type { StartAudio, StartVideo } from '#domain/protocol/messages.ts';
import { sameParams, type DesiredStreams } from '#domain/session/StreamReconciler.ts';

export interface StreamCoordinatorOptions {
  /** Video parameters until the first `setVideoParams`. */
  readonly video: StartVideo;
  /** Null when the virtual microphone is unavailable. */
  readonly audio: StartAudio | null;
  readonly onChange: (desired: DesiredStreams) => void;
}

/**
 * Maps the two demand signals (camera consumers, microphone capture) and the streamed camera
 * mode to desired device streams.
 */
export class StreamCoordinator {
  readonly #options: StreamCoordinatorOptions;
  #video: StartVideo;
  #videoDemand = false;
  #audioDemand = false;

  constructor(options: StreamCoordinatorOptions) {
    this.#options = options;
    this.#video = options.video;
  }

  get desired(): DesiredStreams {
    return {
      video: this.#videoDemand ? this.#video : null,
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

  /** New video parameters; a running stream is reconfigured (the device gets a new StartVideo). */
  setVideoParams(params: StartVideo): void {
    if (sameParams(params, this.#video)) return;
    this.#video = params;
    if (this.#videoDemand) this.#options.onChange(this.desired);
  }
}

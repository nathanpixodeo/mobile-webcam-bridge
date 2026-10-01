import type { Clock } from '#domain/clock.ts';
import type { AudioDriftCompensator } from '#domain/audio/AudioDriftCompensator.ts';
import { LinearResampler } from '#domain/audio/LinearResampler.ts';
import type { AudioSink, AudioSinkStatus } from '#ports/Audio.ts';
import type { Metrics } from '#ports/Metrics.ts';
import type { AudioChunk } from './PeerConnection.ts';

export interface AudioPipelineOptions {
  readonly sink: AudioSink;
  readonly compensator: AudioDriftCompensator;
  readonly clock: Clock;
  readonly metrics: Metrics;
}

/**
 * Device PCM → drift-compensating resampler → virtual microphone. The compensator watches the
 * driver's buffer fill (sink status) and steers the resampling ratio; underruns and overshoots
 * are fixed by inserting silence or dropping input.
 */
export class AudioPipeline {
  readonly #options: AudioPipelineOptions;
  readonly #resampler = new LinearResampler();
  #ratio = 1;
  #framesToDrop = 0;
  #lastSeq: number | undefined;

  constructor(options: AudioPipelineOptions) {
    this.#options = options;
  }

  onAudioChunk(chunk: AudioChunk): void {
    const { metrics, sink } = this.#options;
    metrics.increment('audio.chunks');
    if (this.#lastSeq !== undefined && chunk.seq !== (this.#lastSeq + 1) >>> 0) metrics.increment('audio.deviceGaps');
    this.#lastSeq = chunk.seq;
    if (chunk.discontinuity) this.#resampler.reset();

    let samples = this.#resampler.process(chunk.samples, this.#ratio);
    if (this.#framesToDrop > 0) {
      const drop = Math.min(this.#framesToDrop, samples.length);
      this.#framesToDrop -= drop;
      samples = samples.subarray(drop);
      metrics.increment('audio.droppedFrames', drop);
    }
    if (samples.length > 0 && !sink.write(samples)) metrics.increment('audio.sinkOverflows');
  }

  onSinkStatus(status: AudioSinkStatus): void {
    const { compensator, clock, metrics, sink } = this.#options;
    const update = compensator.update(status, clock.nowMs());
    this.#ratio = update.ratio;
    metrics.gauge('audio.fillMs', status.bufferedBytes / ((sink.sampleRate * 2) / 1000));
    metrics.gauge('audio.driftPpm', Math.round(compensator.deviationPpm));
    switch (update.correction.kind) {
      case 'insertSilence':
        if (update.correction.frames > 0) {
          sink.write(new Int16Array(update.correction.frames));
          metrics.increment('audio.silenceFrames', update.correction.frames);
        }
        break;
      case 'dropInput':
        this.#framesToDrop += update.correction.frames;
        break;
      case 'none':
        break;
    }
  }

  /** Forget stream continuity (new connection or new audio configuration). */
  reset(): void {
    this.#resampler.reset();
    this.#framesToDrop = 0;
    this.#lastSeq = undefined;
  }
}

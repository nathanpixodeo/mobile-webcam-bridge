import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { AudioDriftCompensator, type DriftCompensatorOptions } from '#domain/audio/AudioDriftCompensator.ts';
import { LinearResampler } from '#domain/audio/LinearResampler.ts';
import { PiController } from '#domain/audio/PiController.ts';

const SAMPLE_RATE = 48_000;
const BYTES_PER_FRAME = 2;

const OPTIONS: DriftCompensatorOptions = {
  sampleRate: SAMPLE_RATE,
  bytesPerFrame: BYTES_PER_FRAME,
  targetFillMs: 40,
  kp: 10,
  ki: 2,
  maxPpm: 1000,
  slewPpmPerSecond: 200,
  smoothing: 0.2,
  resyncThresholdMs: 150,
};

describe('PiController', () => {
  it('clamps output and stops integrating while saturated', () => {
    const pi = new PiController({ kp: 1, ki: 1, outputMin: -10, outputMax: 10 });
    for (let i = 0; i < 100; i++) assert.equal(pi.update(100, 1), 10);
    // Without anti-windup the integral would be ~10 000 and the output would stay saturated.
    assert.ok(pi.update(-5, 1) < 10);
  });
});

describe('LinearResampler', () => {
  it('passes samples through at ratio 1 with a one-sample delay', () => {
    const resampler = new LinearResampler();
    const first = resampler.process(Int16Array.from([10, 20, 30]), 1);
    const second = resampler.process(Int16Array.from([40, 50]), 1);
    assert.deepEqual([...first, ...second], [0, 10, 20, 30, 40, 50]);
  });

  it('produces ratio × input samples over a long stream, continuous across chunks', () => {
    const resampler = new LinearResampler();
    const ratio = 1.0005;
    let produced = 0;
    let consumed = 0;
    let last: number | undefined;
    for (let chunk = 0; chunk < 1000; chunk++) {
      const input = Int16Array.from({ length: 480 }, (_, i) =>
        Math.round(1000 * Math.sin((2 * Math.PI * (consumed + i)) / 4800)),
      );
      const output = resampler.process(input, ratio);
      if (last !== undefined && output.length > 0) {
        assert.ok(Math.abs((output[0] ?? 0) - last) < 10, `discontinuity at chunk ${chunk}`);
      }
      last = output.at(-1);
      produced += output.length;
      consumed += input.length;
    }
    assert.ok(Math.abs(produced / consumed - ratio) < 1e-5, `measured ratio ${produced / consumed}`);
  });

  it('rejects absurd ratios', () => {
    assert.throws(() => new LinearResampler().process(new Int16Array(4), 3), RangeError);
  });
});

/**
 * Closed-loop simulation: the iPhone produces audio at (1 + driftPpm) × nominal rate, the
 * resampler applies the compensator's ratio, the driver consumes at exactly the nominal rate.
 */
function simulate(driftPpm: number, seconds: number): { finalFillMs: number; maxPpm: number; corrections: number } {
  const compensator = new AudioDriftCompensator(OPTIONS);
  const updateEveryMs = 100;
  let bufferedFrames = 0;
  let nowMs = 0;
  let ratio = 1;
  let maxPpm = 0;
  let corrections = 0;
  let produceFraction = 0;

  for (let step = 0; step < (seconds * 1000) / updateEveryMs; step++) {
    const producedExact = ((SAMPLE_RATE * updateEveryMs) / 1000) * (1 + driftPpm * 1e-6) * ratio + produceFraction;
    const produced = Math.floor(producedExact);
    produceFraction = producedExact - produced;
    const consumed = (SAMPLE_RATE * updateEveryMs) / 1000;
    bufferedFrames = Math.max(0, bufferedFrames + produced - consumed);
    nowMs += updateEveryMs;

    const update = compensator.update(
      { bufferedBytes: bufferedFrames * BYTES_PER_FRAME, streamActive: true, underruns: 0 },
      nowMs,
    );
    ratio = update.ratio;
    maxPpm = Math.max(maxPpm, Math.abs(compensator.deviationPpm));
    if (update.correction.kind === 'insertSilence') {
      bufferedFrames += update.correction.frames;
      corrections++;
    } else if (update.correction.kind === 'dropInput') {
      bufferedFrames -= update.correction.frames;
      corrections++;
    }
  }
  return { finalFillMs: (bufferedFrames / SAMPLE_RATE) * 1000, maxPpm, corrections };
}

describe('AudioDriftCompensator', () => {
  for (const drift of [-300, -100, 0, 100, 300]) {
    it(`holds the buffer near target with ${drift} ppm clock drift`, () => {
      const result = simulate(drift, 600);
      assert.ok(Math.abs(result.finalFillMs - OPTIONS.targetFillMs) < 5, `final fill ${result.finalFillMs} ms`);
      assert.ok(result.maxPpm <= OPTIONS.maxPpm);
      assert.equal(result.corrections, 1, 'only the start-up prefill');
    });
  }

  it('prefills when capture starts and idles while inactive', () => {
    const compensator = new AudioDriftCompensator(OPTIONS);
    assert.deepEqual(compensator.update({ bufferedBytes: 0, streamActive: false, underruns: 0 }, 0), {
      ratio: 1,
      correction: { kind: 'none' },
    });
    const start = compensator.update({ bufferedBytes: 0, streamActive: true, underruns: 0 }, 100);
    assert.deepEqual(start.correction, { kind: 'insertSilence', frames: 1920 });
  });

  it('inserts silence after an underrun and drops input on large overshoot', () => {
    const compensator = new AudioDriftCompensator(OPTIONS);
    compensator.update({ bufferedBytes: 0, streamActive: true, underruns: 0 }, 0);
    const underrun = compensator.update({ bufferedBytes: 0, streamActive: true, underruns: 1 }, 100);
    assert.equal(underrun.correction.kind, 'insertSilence');
    const overshoot = compensator.update({ bufferedBytes: 300 * 96, streamActive: true, underruns: 1 }, 200);
    assert.deepEqual(overshoot.correction, { kind: 'dropInput', frames: 260 * 48 });
  });
});

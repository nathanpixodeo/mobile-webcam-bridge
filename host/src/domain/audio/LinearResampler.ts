/**
 * Streaming linear-interpolation resampler for mono 16-bit PCM with a ratio that may change on
 * every call. Phase and the last input sample carry over between chunks, so the output is
 * continuous across chunk boundaries. At deviations of a few hundred ppm, linear interpolation
 * is transparent for speech; it is not meant for large rate conversions.
 */
export class LinearResampler {
  /** Position of the next output sample, in input samples, relative to the previous chunk's last sample. */
  #position = 0;
  #previous = 0;

  /**
   * @param input chunk of mono s16 samples
   * @param ratio output samples per input sample (e.g. 1.0002)
   */
  process(input: Int16Array, ratio: number): Int16Array {
    if (!(ratio > 0.5 && ratio < 2)) throw new RangeError(`Unsupported resampling ratio ${ratio}`);
    const length = input.length;
    if (length === 0) return new Int16Array(0);

    const step = 1 / ratio;
    // Upper bound on the output count, with slack for floating-point accumulation in `t`.
    const output = new Int16Array(Math.ceil((length + 1) * ratio) + 2);
    let written = 0;
    // Virtual sequence: index -1 is `#previous`, indices 0..length-1 are `input`.
    let t = this.#position - 1;
    while (t <= length - 1) {
      const i = Math.floor(t);
      const frac = t - i;
      const a = i < 0 ? this.#previous : (input[i] ?? 0);
      const b = i + 1 <= length - 1 ? (input[i + 1] ?? 0) : a;
      output[written++] = clampToInt16(a + (b - a) * frac);
      t += step;
    }
    this.#position = t - (length - 1);
    this.#previous = input[length - 1] ?? 0;
    return output.subarray(0, written);
  }

  reset(): void {
    this.#position = 0;
    this.#previous = 0;
  }
}

function clampToInt16(value: number): number {
  const rounded = Math.round(value);
  return rounded > 32767 ? 32767 : rounded < -32768 ? -32768 : rounded;
}

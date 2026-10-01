export interface PiControllerOptions {
  readonly kp: number;
  readonly ki: number;
  readonly outputMin: number;
  readonly outputMax: number;
}

/**
 * Proportional–integral controller with clamped output and conditional-integration anti-windup
 * (the integrator freezes while the output is saturated in the direction of the error).
 */
export class PiController {
  readonly #options: PiControllerOptions;
  #integral = 0;

  constructor(options: PiControllerOptions) {
    if (options.outputMin >= options.outputMax) throw new RangeError('outputMin must be < outputMax');
    this.#options = options;
  }

  update(error: number, dtSeconds: number): number {
    const { kp, ki, outputMin, outputMax } = this.#options;
    const candidateIntegral = this.#integral + error * dtSeconds;
    const unclamped = kp * error + ki * candidateIntegral;
    const output = Math.min(outputMax, Math.max(outputMin, unclamped));
    const saturatedHigh = unclamped > outputMax && error > 0;
    const saturatedLow = unclamped < outputMin && error < 0;
    if (!saturatedHigh && !saturatedLow) this.#integral = candidateIntegral;
    return output;
  }

  reset(): void {
    this.#integral = 0;
  }
}

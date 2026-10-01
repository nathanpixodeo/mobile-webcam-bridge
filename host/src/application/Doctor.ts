import { toError } from '#domain/errors.ts';
import type { DoctorCheckResult } from '#ports/NativeHelper.ts';

/** One environment check. Checks are independent; a failing check never aborts the others. */
export interface DoctorCheck {
  readonly id: string;
  run(): Promise<DoctorCheckResult | readonly DoctorCheckResult[]>;
}

export class Doctor {
  readonly #checks: readonly DoctorCheck[];

  constructor(checks: readonly DoctorCheck[]) {
    this.#checks = checks;
  }

  async run(): Promise<DoctorCheckResult[]> {
    const results: DoctorCheckResult[] = [];
    for (const check of this.#checks) {
      try {
        const outcome = await check.run();
        results.push(...(isResultList(outcome) ? outcome : [outcome]));
      } catch (error) {
        results.push({ id: check.id, status: 'fail', message: toError(error).message });
      }
    }
    return results;
  }
}

/** Adapts a function to a `DoctorCheck`. */
export function check(id: string, run: () => Promise<DoctorCheckResult | readonly DoctorCheckResult[]>): DoctorCheck {
  return { id, run };
}

function isResultList(
  outcome: DoctorCheckResult | readonly DoctorCheckResult[],
): outcome is readonly DoctorCheckResult[] {
  return Array.isArray(outcome);
}

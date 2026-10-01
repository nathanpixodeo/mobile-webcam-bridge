/** Helpers around `AbortSignal` shared by every layer. */

/** The signal's reason as an `Error` (an AbortError `DOMException` when none was given). */
export function abortReason(signal: AbortSignal | undefined): Error {
  const reason: unknown = signal?.reason;
  return reason instanceof Error ? reason : new DOMException('The operation was aborted', 'AbortError');
}

/**
 * Reads `signal.aborted` through a call so TypeScript does not keep a stale narrowing across
 * `await`s inside loops guarded by `while (!signal.aborted)`.
 */
export function isAborted(signal: AbortSignal): boolean {
  return signal.aborted;
}

/** A promise that rejects with the abort reason once `signal` aborts. */
export function whenAborted(signal: AbortSignal): Promise<never> {
  return new Promise<never>((_, reject) => {
    if (signal.aborted) {
      reject(abortReason(signal));
      return;
    }
    signal.addEventListener(
      'abort',
      () => {
        reject(abortReason(signal));
      },
      { once: true },
    );
  });
}

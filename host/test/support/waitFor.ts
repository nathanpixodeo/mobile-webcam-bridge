import { setTimeout as sleep } from 'node:timers/promises';

/** Polls `condition` until it holds or `timeoutMs` elapses (real time; integration tests). */
export async function waitFor(condition: () => boolean, description: string, timeoutMs = 5000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (!condition()) {
    if (Date.now() > deadline) throw new Error(`Timed out after ${timeoutMs} ms waiting for: ${description}`);
    await sleep(20);
  }
}

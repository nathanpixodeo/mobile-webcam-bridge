import { setTimeout as sleep } from 'node:timers/promises';
import type { Clock } from '#domain/clock.ts';

export class SystemClock implements Clock {
  nowMs(): number {
    return performance.now();
  }

  nowUs(): bigint {
    return process.hrtime.bigint() / 1000n;
  }

  async sleep(ms: number, signal?: AbortSignal): Promise<void> {
    await sleep(ms, undefined, signal === undefined ? undefined : { signal });
  }

  setTimeout(callback: () => void, ms: number): Disposable {
    const handle = setTimeout(callback, ms);
    return {
      [Symbol.dispose]: () => {
        clearTimeout(handle);
      },
    };
  }

  setInterval(callback: () => void, ms: number): Disposable {
    const handle = setInterval(callback, ms);
    return {
      [Symbol.dispose]: () => {
        clearInterval(handle);
      },
    };
  }
}

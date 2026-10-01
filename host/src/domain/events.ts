/**
 * Minimal typed event emitter. Listeners are registered with `on()`, which returns a
 * `Disposable`, so subscriptions compose with `using` / `DisposableStack`.
 *
 * Event maps are interfaces whose members are argument tuples, e.g.
 * `interface Events { state: [state: State]; closed: [] }`.
 */
import { abortReason } from './abort.ts';

export type EventMap<M> = { readonly [K in keyof M]: readonly unknown[] };

export type Listener<Args extends readonly unknown[]> = (...args: Args) => void;

export interface EventSource<M extends EventMap<M>> {
  on<K extends keyof M & string>(event: K, listener: Listener<M[K]>): Disposable;
}

export interface EmitterOptions {
  /** Called when a listener throws. Defaults to rethrowing on a fresh microtask (fail loudly). */
  readonly onListenerError?: (error: unknown, event: string) => void;
}

export class Emitter<M extends EventMap<M>> implements EventSource<M> {
  readonly #listeners = new Map<keyof M, Set<Listener<never>>>();
  readonly #onListenerError: (error: unknown, event: string) => void;

  constructor(options: EmitterOptions = {}) {
    this.#onListenerError =
      options.onListenerError ??
      ((error) => {
        queueMicrotask(() => {
          throw error;
        });
      });
  }

  on<K extends keyof M & string>(event: K, listener: Listener<M[K]>): Disposable {
    let set = this.#listeners.get(event);
    if (set === undefined) {
      set = new Set();
      this.#listeners.set(event, set);
    }
    const stored = listener as Listener<never>;
    set.add(stored);
    return {
      [Symbol.dispose]: () => {
        set.delete(stored);
      },
    };
  }

  emit<K extends keyof M & string>(event: K, ...args: M[K]): void {
    const set = this.#listeners.get(event);
    if (set === undefined) return;
    for (const listener of [...set]) {
      try {
        (listener as Listener<M[K]>)(...args);
      } catch (error) {
        this.#onListenerError(error, event);
      }
    }
  }

  listenerCount(event: keyof M & string): number {
    return this.#listeners.get(event)?.size ?? 0;
  }

  clear(): void {
    this.#listeners.clear();
  }
}

/** Resolves with the arguments of the next `event`, or rejects when `signal` aborts. */
export function nextEvent<M extends EventMap<M>, K extends keyof M & string>(
  source: EventSource<M>,
  event: K,
  signal?: AbortSignal,
): Promise<M[K]> {
  return new Promise<M[K]>((resolve, reject) => {
    if (signal?.aborted === true) {
      reject(abortReason(signal));
      return;
    }
    const onAbort = (): void => {
      subscription[Symbol.dispose]();
      reject(abortReason(signal));
    };
    const subscription = source.on(event, (...args) => {
      subscription[Symbol.dispose]();
      signal?.removeEventListener('abort', onAbort);
      resolve(args);
    });
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

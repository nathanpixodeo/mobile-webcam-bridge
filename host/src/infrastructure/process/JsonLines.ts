import { createInterface } from 'node:readline';
import type { Readable, Writable } from 'node:stream';
import type * as z from 'zod';

/**
 * Reads newline-delimited JSON from `input`, validating each object against `schema`.
 * Invalid lines are reported through `onInvalid` and skipped.
 */
export function readJsonLines<S extends z.ZodType>(
  input: Readable,
  schema: S,
  onValue: (value: z.infer<S>) => void,
  onInvalid: (line: string, reason: string) => void,
): void {
  createInterface({ input, crlfDelay: Number.POSITIVE_INFINITY }).on('line', (line) => {
    if (line.trim() === '') return;
    let raw: unknown;
    try {
      raw = JSON.parse(line);
    } catch {
      onInvalid(line, 'not JSON');
      return;
    }
    const result = schema.safeParse(raw);
    if (result.success) onValue(result.data);
    else onInvalid(line, result.error.message);
  });
}

/** Writes one JSON object followed by a newline. Returns the stream's backpressure signal. */
export function writeJsonLine(output: Writable, value: unknown): boolean {
  return output.write(`${JSON.stringify(value)}\n`);
}

/**
 * Canonical JSON as required by protocol/SPEC.md §4: object keys sorted by code point, no
 * insignificant whitespace, integers only. Canonical form makes payloads byte-identical across
 * the TypeScript and Swift implementations, which the golden test vectors rely on.
 */

export type JsonValue =
  string | number | boolean | null | readonly JsonValue[] | { readonly [key: string]: JsonValue | undefined };

export function canonicalJson(value: JsonValue): string {
  if (value === null || typeof value === 'boolean' || typeof value === 'string') return JSON.stringify(value);
  if (typeof value === 'number') {
    if (!Number.isSafeInteger(value)) throw new TypeError(`Canonical JSON allows only safe integers, got ${value}`);
    return String(value);
  }
  if (isArray(value)) return `[${value.map(canonicalJson).join(',')}]`;
  const entries = Object.entries(value)
    .filter((entry): entry is [string, JsonValue] => entry[1] !== undefined)
    .sort(([a], [b]) => compareCodePoints(a, b));
  return `{${entries.map(([key, item]) => `${JSON.stringify(key)}:${canonicalJson(item)}`).join(',')}}`;
}

export function encodeCanonicalJson(value: JsonValue): Buffer {
  return Buffer.from(canonicalJson(value), 'utf8');
}

function isArray(value: JsonValue): value is readonly JsonValue[] {
  return Array.isArray(value);
}

/** Orders strings by Unicode code point (not UTF-16 unit, not locale). */
function compareCodePoints(a: string, b: string): number {
  let i = 0;
  let j = 0;
  while (i < a.length && j < b.length) {
    const left = a.codePointAt(i) ?? 0;
    const right = b.codePointAt(j) ?? 0;
    if (left !== right) return left - right;
    i += left > 0xffff ? 2 : 1;
    j += right > 0xffff ? 2 : 1;
  }
  return a.length - i - (b.length - j);
}

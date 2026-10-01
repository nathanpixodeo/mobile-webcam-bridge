import { existsSync, readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import * as z from 'zod';
import { ConfigError } from '#domain/errors.ts';
import { ConfigSchema, type BridgeConfig } from './ConfigSchema.ts';

export interface LoadedConfig {
  readonly config: BridgeConfig;
  /** File the configuration came from, if any. */
  readonly source: string | undefined;
}

type JsonObject = Record<string, unknown>;

/**
 * Loads configuration with precedence defaults < file < overrides (CLI flags). Without an
 * explicit path it looks for `bridge.config.json` in the working directory, then
 * `%APPDATA%\mobile-webcam-bridge\config.json`.
 */
export class ConfigLoader {
  readonly #cwd: string;
  readonly #appData: string | undefined;

  constructor(options: { cwd?: string; appData?: string | undefined } = {}) {
    this.#cwd = options.cwd ?? process.cwd();
    this.#appData = options.appData ?? process.env['APPDATA'];
  }

  load(explicitPath: string | undefined, overrides: JsonObject = {}): LoadedConfig {
    const source = this.#resolvePath(explicitPath);
    const fileValues = source === undefined ? {} : readJsonFile(source);
    const merged = deepMerge(fileValues, overrides);
    const result = ConfigSchema.safeParse(merged);
    if (!result.success) {
      throw new ConfigError(
        `Invalid configuration${source === undefined ? '' : ` in ${source}`}:\n${z.prettifyError(result.error)}`,
      );
    }
    return { config: result.data, source };
  }

  #resolvePath(explicitPath: string | undefined): string | undefined {
    if (explicitPath !== undefined) {
      const path = resolve(this.#cwd, explicitPath);
      if (!existsSync(path)) throw new ConfigError(`Configuration file not found: ${path}`);
      return path;
    }
    const candidates = [join(this.#cwd, 'bridge.config.json')];
    if (this.#appData !== undefined) candidates.push(join(this.#appData, 'mobile-webcam-bridge', 'config.json'));
    return candidates.find((candidate) => existsSync(candidate));
  }
}

function readJsonFile(path: string): JsonObject {
  let value: unknown;
  try {
    value = JSON.parse(readFileSync(path, 'utf8'));
  } catch (error) {
    throw new ConfigError(`Cannot read configuration ${path}: ${(error as Error).message}`, { cause: error });
  }
  if (!isObject(value)) throw new ConfigError(`Configuration ${path} must contain a JSON object`);
  return value;
}

function deepMerge(base: JsonObject, override: JsonObject): JsonObject {
  const out: JsonObject = { ...base };
  for (const [key, value] of Object.entries(override)) {
    if (value === undefined) continue;
    const current = out[key];
    out[key] = isObject(current) && isObject(value) ? deepMerge(current, value) : value;
  }
  return out;
}

function isObject(value: unknown): value is JsonObject {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

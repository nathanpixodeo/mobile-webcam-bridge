import { existsSync, readdirSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const EXECUTABLE = 'bridge-native.exe';

/**
 * Finds bridge-native.exe, in order: explicit configuration, the newest installed version under
 * `%ProgramFiles%\MobileWebcamBridge\<version>\`, then the repository build stage (development).
 */
export function locateBridgeNative(configuredPath: string | undefined): string | undefined {
  if (configuredPath !== undefined) return existsSync(configuredPath) ? configuredPath : undefined;
  return newestInstalled() ?? stageBuild('Release') ?? stageBuild('Debug');
}

/** Stage folder of a repository build, used by `install` as its copy source. */
export function stageBuild(configuration: 'Release' | 'Debug'): string | undefined {
  const repoRoot = resolve(fileURLToPath(new URL('../../../../', import.meta.url)));
  const candidate = join(repoRoot, 'native', 'out', configuration, 'stage', EXECUTABLE);
  return existsSync(candidate) ? candidate : undefined;
}

function newestInstalled(): string | undefined {
  const programFiles = process.env['ProgramFiles'];
  if (programFiles === undefined) return undefined;
  const root = join(programFiles, 'MobileWebcamBridge');
  let versions: string[];
  try {
    versions = readdirSync(root, { withFileTypes: true })
      .filter((entry) => entry.isDirectory() && /^\d+\.\d+\.\d+$/.test(entry.name))
      .map((entry) => entry.name);
  } catch {
    return undefined;
  }
  versions.sort(compareVersions).reverse();
  for (const version of versions) {
    const candidate = join(root, version, EXECUTABLE);
    if (existsSync(candidate)) return candidate;
  }
  return undefined;
}

function compareVersions(a: string, b: string): number {
  const left = a.split('.').map(Number);
  const right = b.split('.').map(Number);
  for (let i = 0; i < 3; i++) {
    const diff = (left[i] ?? 0) - (right[i] ?? 0);
    if (diff !== 0) return diff;
  }
  return 0;
}

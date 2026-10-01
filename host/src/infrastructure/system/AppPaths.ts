import { homedir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

/** Filesystem locations used by the host. */
export const AppPaths = {
  /** Per-user writable data (logs, caches). */
  dataDir(): string {
    return join(process.env['LOCALAPPDATA'] ?? join(homedir(), 'AppData', 'Local'), 'mobile-webcam-bridge');
  },
  logsDir(): string {
    return join(AppPaths.dataDir(), 'logs');
  },
  placeholderCacheDir(): string {
    return join(AppPaths.dataDir(), 'cache', 'placeholders');
  },
  /** Placeholder PNGs shipped with the host. */
  placeholderAssetsDir(): string {
    return fileURLToPath(new URL('../../../assets/placeholders/', import.meta.url));
  },
} as const;

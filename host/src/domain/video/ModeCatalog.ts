import { sameSize, type VideoMode, type VideoSize } from './VideoMode.ts';

/**
 * The camera mode catalog (protocol/FRAME_PIPE.md §1, mirrored by native/common FrameProtocol.h):
 * every size at 15, 30 and 60 fps, except 3840x2160, which stops at 30 fps (raw NV12 at 4K60 is
 * about 750 MB/s per consumer). The installed cap filters what the camera advertises.
 */
export const CATALOG_SIZES: readonly VideoSize[] = [
  { width: 640, height: 360 },
  { width: 640, height: 480 },
  { width: 960, height: 540 },
  { width: 1280, height: 720 },
  { width: 1920, height: 1080 },
  { width: 2560, height: 1440 },
  { width: 3840, height: 2160 },
];

export const CATALOG_FRAME_RATES: readonly number[] = [15, 30, 60];

const MAX_FPS_ABOVE_1440P = 30;

/** The installed limit on advertised modes (`install --max-width --max-height --max-fps`). */
export interface ModeCap {
  readonly maxWidth: number;
  readonly maxHeight: number;
  readonly maxFps: number;
}

export function isCatalogSize(size: VideoSize): boolean {
  return CATALOG_SIZES.some((candidate) => sameSize(candidate, size));
}

/** Highest catalog frame rate for `size`, or undefined when `size` is not in the catalog. */
export function maxFpsFor(size: VideoSize): number | undefined {
  if (!isCatalogSize(size)) return undefined;
  return size.width * size.height > 2560 * 1440 ? MAX_FPS_ABOVE_1440P : 60;
}

export function isCatalogMode(mode: VideoMode): boolean {
  const maxFps = maxFpsFor(mode);
  return (
    maxFps !== undefined && mode.fpsDen === 1 && CATALOG_FRAME_RATES.includes(mode.fpsNum) && mode.fpsNum <= maxFps
  );
}

/** Largest side and pixel count of a frame on the hub's ingest pipe (a 4K frame, either way up). */
const MAX_INGEST_SIDE = 3840;
const MAX_INGEST_PIXELS = 3840 * 2160;

/**
 * True for a size the hub accepts on its ingest pipe: even sides (NV12), at most 3840 on either
 * side and 3840x2160 pixels. Not limited to the catalog, so the decoder can keep the size the
 * phone encodes (portrait, or a size it fell back to) and leave the scaling to the hub.
 */
export function isIngestSize(size: VideoSize): boolean {
  const { width, height } = size;
  return (
    Number.isInteger(width) &&
    Number.isInteger(height) &&
    width >= 2 &&
    height >= 2 &&
    width % 2 === 0 &&
    height % 2 === 0 &&
    width <= MAX_INGEST_SIDE &&
    height <= MAX_INGEST_SIDE &&
    width * height <= MAX_INGEST_PIXELS
  );
}

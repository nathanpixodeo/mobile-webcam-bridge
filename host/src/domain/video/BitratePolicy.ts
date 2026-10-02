import { framesPerSecond, type VideoMode } from './VideoMode.ts';

export const MIN_BITRATE_KBPS = 1500;
/** The protocol maximum for `StartVideo.bitrateKbps`. */
export const MAX_BITRATE_KBPS = 40_000;

export interface BitratePolicyOptions {
  /** H.264 bits per pixel per frame (0.1 gives about 6.2 Mbit/s at 1920x1080@30). */
  readonly bitsPerPixel: number;
  /** Fixed bitrate configured by the user, used for every mode. */
  readonly overrideKbps?: number | undefined;
}

/** Encoder bitrate for streaming `mode`: proportional to the pixel rate, within sane bounds. */
export function bitrateKbpsFor(mode: VideoMode, options: BitratePolicyOptions): number {
  if (options.overrideKbps !== undefined) return options.overrideKbps;
  const kbps = Math.round((mode.width * mode.height * framesPerSecond(mode) * options.bitsPerPixel) / 1000);
  return Math.min(MAX_BITRATE_KBPS, Math.max(MIN_BITRATE_KBPS, kbps));
}

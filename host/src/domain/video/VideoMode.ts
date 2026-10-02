/**
 * A camera mode: frame size and rate. The virtual camera advertises a catalog of them
 * (`ModeCatalog.ts`); each consumer subscribes to one, and the phone streams the mode the
 * consumers need (`ModeArbiter.ts`).
 */
export interface VideoMode {
  readonly width: number;
  readonly height: number;
  readonly fpsNum: number;
  readonly fpsDen: number;
}

export type VideoSize = Pick<VideoMode, 'width' | 'height'>;

export function framesPerSecond(mode: VideoMode): number {
  return mode.fpsNum / mode.fpsDen;
}

export function nv12FrameBytes(size: VideoSize): number {
  return (size.width * size.height * 3) / 2;
}

export function modeArea(size: VideoSize): number {
  return size.width * size.height;
}

/** Orders modes by frame area, then by frame rate (negative when `a` is smaller). */
export function compareModes(a: VideoMode, b: VideoMode): number {
  return modeArea(a) - modeArea(b) || framesPerSecond(a) - framesPerSecond(b);
}

export function sameSize(a: VideoSize, b: VideoSize): boolean {
  return a.width === b.width && a.height === b.height;
}

export function sameMode(a: VideoMode, b: VideoMode): boolean {
  return sameSize(a, b) && a.fpsNum * b.fpsDen === b.fpsNum * a.fpsDen;
}

export function describeSize(size: VideoSize): string {
  return `${size.width}x${size.height}`;
}

export function describeMode(mode: VideoMode): string {
  const fps = framesPerSecond(mode);
  return `${describeSize(mode)}@${Number.isInteger(fps) ? fps : fps.toFixed(2)}`;
}

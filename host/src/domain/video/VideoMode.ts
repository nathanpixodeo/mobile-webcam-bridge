/** The fixed mode the virtual camera advertises (installed by bridge-native, stored in HKLM). */
export interface VideoMode {
  readonly width: number;
  readonly height: number;
  readonly fpsNum: number;
  readonly fpsDen: number;
}

export function framesPerSecond(mode: VideoMode): number {
  return mode.fpsNum / mode.fpsDen;
}

export function nv12FrameBytes(mode: Pick<VideoMode, 'width' | 'height'>): number {
  return (mode.width * mode.height * 3) / 2;
}

export function describeMode(mode: VideoMode): string {
  const fps = framesPerSecond(mode);
  return `${mode.width}x${mode.height}@${Number.isInteger(fps) ? fps : fps.toFixed(2)}`;
}

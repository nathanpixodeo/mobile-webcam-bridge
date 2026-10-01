import type { VideoMode } from '#domain/video/VideoMode.ts';

export type HardwareAcceleration = 'none' | 'd3d11va' | 'dxva2';

export interface DecoderArgsOptions {
  readonly mode: Pick<VideoMode, 'width' | 'height'>;
  readonly output: string;
  readonly hwaccel: HardwareAcceleration;
  readonly logLevel?: 'quiet' | 'error' | 'warning' | 'info';
}

/**
 * ffmpeg arguments for the low-latency decoder: H.264 Annex-B on stdin, raw NV12 frames of
 * exactly the camera mode to `output` (the hub's ingest pipe).
 *
 * - `low_delay` + tiny probe: emit frames as soon as they are decodable. Do NOT add
 *   `-fflags nobuffer`: with raw H.264 on a pipe it drops half the frames and adds >1 s of
 *   latency (measured with ffmpeg 8.1). Measured decode latency with these flags and the
 *   trailing AUD: ~1 frame interval + ~14 ms.
 * - `-fps_mode passthrough`: the rawvideo muxer would otherwise run CFR and dup/drop frames.
 * - scale+pad: letterbox any input size (portrait, other modes) into the fixed output; a no-op
 *   when sizes match. BT.709 limited range matches what the camera components advertise.
 * - `-flush_packets 1`: write each frame to the pipe immediately.
 */
export function buildDecoderArgs(options: DecoderArgsOptions): string[] {
  const { width, height } = options.mode;
  const filter = [
    `scale=${width}:${height}:force_original_aspect_ratio=decrease:force_divisible_by=2:out_color_matrix=bt709:out_range=tv`,
    `pad=${width}:${height}:(ow-iw)/2:(oh-ih)/2:color=black`,
    'format=nv12',
  ].join(',');
  return [
    '-hide_banner',
    '-nostdin',
    '-loglevel',
    options.logLevel ?? 'error',
    ...(options.hwaccel === 'none' ? [] : ['-hwaccel', options.hwaccel]),
    '-flags',
    'low_delay',
    '-probesize',
    '32',
    '-analyzeduration',
    '0',
    '-threads',
    '1',
    '-f',
    'h264',
    '-i',
    'pipe:0',
    '-map',
    '0:v:0',
    '-an',
    '-fps_mode',
    'passthrough',
    '-vf',
    filter,
    '-f',
    'rawvideo',
    '-flush_packets',
    '1',
    '-y',
    options.output,
  ];
}

export interface PlaceholderArgsOptions {
  readonly input: string;
  readonly mode: Pick<VideoMode, 'width' | 'height'>;
  readonly output: string;
}

/** ffmpeg arguments that convert a placeholder image into one raw NV12 frame. */
export function buildPlaceholderArgs(options: PlaceholderArgsOptions): string[] {
  const { width, height } = options.mode;
  return [
    '-hide_banner',
    '-nostdin',
    '-loglevel',
    'error',
    '-i',
    options.input,
    '-vf',
    `scale=${width}:${height}:force_original_aspect_ratio=decrease:out_color_matrix=bt709:out_range=tv,pad=${width}:${height}:(ow-iw)/2:(oh-ih)/2:color=black,format=nv12`,
    '-frames:v',
    '1',
    '-f',
    'rawvideo',
    '-y',
    options.output,
  ];
}

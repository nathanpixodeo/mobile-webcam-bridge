import type { VideoSize } from '#domain/video/VideoMode.ts';

export type HardwareAcceleration = 'none' | 'd3d11va' | 'dxva2';
/** The configured choice; `auto` is resolved per decode mode. */
export type HardwareAccelerationSetting = 'auto' | HardwareAcceleration;

/**
 * `auto` decodes up to 1080p in software, which keeps latency lowest (no GPU round trip), and
 * larger modes with D3D11VA: the single software decoding thread (`-threads 1`, which avoids
 * frame-threading delay) does not keep up with 1440p or 4K.
 */
export function resolveHardwareAcceleration(
  setting: HardwareAccelerationSetting,
  mode: VideoSize,
): HardwareAcceleration {
  if (setting !== 'auto') return setting;
  return mode.height > 1080 ? 'd3d11va' : 'none';
}

export interface DecoderArgsOptions {
  /** Size of the frames written to `output` (the hub's ingest size). */
  readonly mode: VideoSize;
  readonly output: string;
  readonly hwaccel: HardwareAcceleration;
  readonly logLevel?: 'quiet' | 'error' | 'warning' | 'info';
}

/**
 * ffmpeg arguments for the low-latency decoder: H.264 Annex-B on stdin, raw NV12 frames of
 * exactly the decode mode to `output` (the hub's ingest pipe).
 *
 * - `low_delay` + tiny probe: emit frames as soon as they are decodable. Do NOT add
 *   `-fflags nobuffer`: with raw H.264 on a pipe it drops half the frames and adds >1 s of
 *   latency (measured with ffmpeg 8.1). Measured decode latency with these flags and the
 *   trailing AUD: ~1 frame interval + ~14 ms.
 * - `-fps_mode passthrough`: the rawvideo muxer would otherwise run CFR and dup/drop frames.
 * - scale+pad: letterbox any input size (portrait, a size the phone could not match) into the
 *   decode mode; a no-op when sizes match. BT.709 limited range matches what the camera
 *   components advertise.
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
  readonly mode: VideoSize;
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

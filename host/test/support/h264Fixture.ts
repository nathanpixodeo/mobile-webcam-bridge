import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { NalType, nalUnitType, splitNalUnits } from '#domain/h264/AnnexB.ts';

export interface FixtureOptions {
  readonly width: number;
  readonly height: number;
  readonly fps: number;
  readonly seconds: number;
}

const START_CODE = Buffer.from([0, 0, 0, 1]);

/** True when an ffmpeg with libx264 is on PATH (integration tests skip otherwise). */
export function ffmpegAvailable(): boolean {
  const result = spawnSync('ffmpeg', ['-hide_banner', '-encoders'], { encoding: 'utf8' });
  return result.status === 0 && result.stdout.includes('libx264');
}

/**
 * Generates (and caches) an H.264 Annex-B test stream shaped like the iPhone's: no B-frames,
 * SPS/PPS before every IDR, one IDR per second.
 */
export function h264Fixture(options: FixtureOptions): Buffer {
  const dir = join(tmpdir(), 'mobile-webcam-bridge-test-fixtures');
  mkdirSync(dir, { recursive: true });
  const path = join(dir, `testsrc-${options.width}x${options.height}-${options.fps}fps-${options.seconds}s.h264`);
  if (!existsSync(path)) {
    const result = spawnSync(
      'ffmpeg',
      [
        '-hide_banner',
        '-loglevel',
        'error',
        '-y',
        '-f',
        'lavfi',
        '-i',
        `testsrc=size=${options.width}x${options.height}:rate=${options.fps}`,
        '-t',
        String(options.seconds),
        '-c:v',
        'libx264',
        '-profile:v',
        'high',
        '-bf',
        '0',
        '-g',
        String(options.fps),
        '-x264-params',
        'repeat-headers=1',
        '-pix_fmt',
        'yuv420p',
        '-f',
        'h264',
        path,
      ],
      { encoding: 'utf8' },
    );
    if (result.status !== 0) throw new Error(`ffmpeg fixture generation failed: ${result.stderr}`);
  }
  return readFileSync(path);
}

/** Groups NAL units into access units (one coded picture each, parameter sets with the IDR). */
export function splitAccessUnits(stream: Buffer): Buffer[] {
  const accessUnits: Buffer[] = [];
  let current: Buffer[] = [];
  let hasPicture = false;
  for (const nal of splitNalUnits(stream)) {
    const type = nalUnitType(nal);
    const isPicture = type === NalType.NonIdrSlice || type === NalType.IdrSlice;
    const startsNewUnit =
      isPicture || type === NalType.Sps || type === NalType.Sei || type === NalType.AccessUnitDelimiter;
    if (hasPicture && startsNewUnit) {
      accessUnits.push(Buffer.concat(current));
      current = [];
      hasPicture = false;
    }
    current.push(START_CODE, Buffer.from(nal));
    if (isPicture) hasPicture = true;
  }
  if (current.length > 0) accessUnits.push(Buffer.concat(current));
  return accessUnits;
}

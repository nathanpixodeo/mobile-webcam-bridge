import assert from 'node:assert/strict';
import { mkdtempSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, it } from 'node:test';
import { PLACEHOLDER_KINDS } from '#domain/video/PlaceholderPolicy.ts';
import { nv12FrameBytes } from '#domain/video/VideoMode.ts';
import { silentLogger } from '#ports/Logger.ts';
import { FfmpegPlaceholderRenderer, PLACEHOLDER_FRAME_SIZE } from '#infrastructure/ffmpeg/FfmpegToolchain.ts';
import { AppPaths } from '#infrastructure/system/AppPaths.ts';
import { ffmpegAvailable } from '#test/support/h264Fixture.ts';

describe('FfmpegPlaceholderRenderer', { skip: !ffmpegAvailable() && 'ffmpeg required' }, () => {
  it('renders every shipped placeholder into a 1920x1080 NV12 frame, and caches it', async () => {
    const cacheDir = mkdtempSync(join(tmpdir(), 'ipb-placeholders-'));
    const renderer = new FfmpegPlaceholderRenderer({
      ffmpegPath: 'ffmpeg',
      assetsDir: AppPaths.placeholderAssetsDir(),
      cacheDir,
      logger: silentLogger,
    });
    const rendered = await renderer.render(PLACEHOLDER_KINDS);
    assert.deepEqual([...rendered.keys()], [...PLACEHOLDER_KINDS]);
    assert.deepEqual(PLACEHOLDER_FRAME_SIZE, { width: 1920, height: 1080 });
    for (const path of rendered.values()) assert.equal(statSync(path).size, nv12FrameBytes(PLACEHOLDER_FRAME_SIZE));

    const firstMtime = statSync(rendered.get('no-device') ?? '').mtimeMs;
    await renderer.render(['no-device']);
    assert.equal(statSync(rendered.get('no-device') ?? '').mtimeMs, firstMtime, 'cached frame reused');
  });
});

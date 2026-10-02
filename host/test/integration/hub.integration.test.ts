import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { constants, createWriteStream, mkdtempSync, openSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, it } from 'node:test';
import { PLACEHOLDER_KINDS, type PlaceholderKind } from '#domain/video/PlaceholderPolicy.ts';
import { nv12FrameBytes, type VideoMode, type VideoSize } from '#domain/video/VideoMode.ts';
import { silentLogger } from '#ports/Logger.ts';
import { FfmpegPlaceholderRenderer, PLACEHOLDER_FRAME_SIZE } from '#infrastructure/ffmpeg/FfmpegToolchain.ts';
import { locateBridgeNative } from '#infrastructure/native/BridgeNativeLocator.ts';
import { VideoHubOutput } from '#infrastructure/native/VideoHubOutput.ts';
import { runToCompletion } from '#infrastructure/process/ManagedChildProcess.ts';
import { AppPaths } from '#infrastructure/system/AppPaths.ts';
import { ffmpegAvailable } from '#test/support/h264Fixture.ts';
import { waitFor } from '#test/support/waitFor.ts';

const executable = locateBridgeNative(undefined);

const HD: VideoMode = { width: 1280, height: 720, fpsNum: 30, fpsDen: 1 };
const VGA: VideoMode = { width: 640, height: 480, fpsNum: 30, fpsDen: 1 };
const DEFAULT_MODE = HD;

interface WatchResult {
  readonly ok: boolean;
  readonly frames: number;
  readonly placeholderFrames: number;
  readonly width: number;
  readonly height: number;
}

/** `bridge-native video watch`: one consumer subscribed to `mode` until it received `frames`. */
async function watch(pipeName: string, mode: VideoMode, frames: number): Promise<WatchResult> {
  const { stdout } = await runToCompletion(
    executable ?? '',
    [
      'video',
      'watch',
      '--pipe-name',
      pipeName,
      '--width',
      String(mode.width),
      '--height',
      String(mode.height),
      '--fps',
      String(mode.fpsNum),
      '--frames',
      String(frames),
      '--timeout-ms',
      '15000',
    ],
    { timeoutMs: 20_000, logger: silentLogger },
  );
  return JSON.parse(stdout.trim()) as WatchResult;
}

function startHub(
  pipeName: string,
  placeholders: ReadonlyMap<PlaceholderKind, string> = new Map(),
): Promise<VideoHubOutput> {
  return VideoHubOutput.start({
    executablePath: executable ?? '',
    logger: silentLogger,
    placeholders,
    placeholderSize: PLACEHOLDER_FRAME_SIZE,
    defaultMode: DEFAULT_MODE,
    cap: { maxWidth: 1920, maxHeight: 1080, maxFps: 60 },
    pipeName,
  });
}

/**
 * Writes grey NV12 frames of `size` into the ingest pipe until stopped. The pipe is inbound-only,
 * so it is opened write-only like ffmpeg does; `net.connect` asks for read access and fails.
 */
function feedFrames(ingestPath: string, size: VideoSize): Disposable {
  const stream = createWriteStream('', { fd: openSync(ingestPath, constants.O_WRONLY) });
  stream.on('error', () => undefined);
  const frame = Buffer.alloc(nv12FrameBytes(size), 128);
  const timer = setInterval(() => {
    if (stream.writableLength < frame.length * 2) stream.write(frame);
  }, 33);
  return {
    [Symbol.dispose]: () => {
      clearInterval(timer);
      stream.destroy();
    },
  };
}

const testPipeName = (): string => `mobile-webcam-bridge-test-${randomBytes(6).toString('hex')}`;

describe(
  'bridge-native video hub (real binary)',
  { skip: executable === undefined && 'bridge-native.exe not built or installed' },
  () => {
    it('reports every consumer with its mode and scales the ingest frames for each one', async () => {
      const pipeName = testPipeName();
      await using hub = await startHub(pipeName);
      assert.deepEqual(hub.defaultMode, DEFAULT_MODE);
      assert.deepEqual(hub.ingestMode, { width: 1280, height: 720 }, 'the default mode is the initial ingest size');

      const first = watch(pipeName, HD, 10);
      await waitFor(() => hub.consumers.count === 1, 'first consumer', 10_000);
      const second = watch(pipeName, VGA, 10);
      await waitFor(() => hub.consumers.count === 2, 'second consumer', 10_000);
      assert.deepEqual(hub.consumers.modes, [HD, VGA], 'one mode per consumer, in subscription order');

      await hub.setIngestMode({ width: 1920, height: 1080 }, AbortSignal.timeout(5000));
      assert.deepEqual(hub.ingestMode, { width: 1920, height: 1080 });

      using _feed = feedFrames(hub.ingestPath, hub.ingestMode);
      const [hd, vga] = await Promise.all([first, second]);
      assert.ok(hd.ok && hd.frames >= 10, `1280x720 consumer received frames: ${JSON.stringify(hd)}`);
      assert.deepEqual([hd.width, hd.height], [1280, 720]);
      assert.ok(vga.ok && vga.frames >= 10, `640x480 consumer received frames: ${JSON.stringify(vga)}`);
      assert.deepEqual([vga.width, vga.height], [640, 480]);
      await waitFor(() => hub.consumers.count === 0, 'consumers gone', 10_000);
    });

    it('ingests the size the phone encodes, even outside the catalog and the cap', async () => {
      await using hub = await startHub(testPipeName());
      await hub.setIngestMode({ width: 1080, height: 1920 }, AbortSignal.timeout(5000));
      assert.deepEqual(hub.ingestMode, { width: 1080, height: 1920 }, 'portrait, taller than the 1080 cap');
      await assert.rejects(hub.setIngestMode({ width: 7680, height: 4320 }, AbortSignal.timeout(5000)));
      assert.deepEqual(hub.ingestMode, { width: 1080, height: 1920 });
    });

    it(
      'scales 1920x1080 placeholders to the consumer mode',
      { skip: !ffmpegAvailable() && 'ffmpeg required' },
      async () => {
        const placeholders = await new FfmpegPlaceholderRenderer({
          ffmpegPath: 'ffmpeg',
          assetsDir: AppPaths.placeholderAssetsDir(),
          cacheDir: mkdtempSync(join(tmpdir(), 'mwb-hub-placeholders-')),
          logger: silentLogger,
        }).render(PLACEHOLDER_KINDS);
        const pipeName = testPipeName();
        await using hub = await startHub(pipeName, placeholders);
        hub.setPlaceholder('no-device');
        const result = await watch(pipeName, VGA, 1);
        assert.ok(result.ok && result.placeholderFrames >= 1, `placeholder frame received: ${JSON.stringify(result)}`);
        assert.deepEqual([result.width, result.height], [640, 480]);
      },
    );
  },
);

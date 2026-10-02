import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, it } from 'node:test';
import { ConfigError } from '#domain/errors.ts';
import { ConfigLoader } from '#infrastructure/config/ConfigLoader.ts';
import { ConfigSchema } from '#infrastructure/config/ConfigSchema.ts';
import { buildDecoderArgs, resolveHardwareAcceleration } from '#infrastructure/ffmpeg/FfmpegArgsBuilder.ts';
import { maxBacklogBytesFor } from '#infrastructure/ffmpeg/FfmpegH264Decoder.ts';
import { HubEventSchema, MicEventSchema, StatusSchema } from '#infrastructure/native/BridgeNativeSchemas.ts';
import { InMemoryMetrics } from '#infrastructure/metrics/InMemoryMetrics.ts';

describe('ConfigSchema', () => {
  it('fills every nested default from an empty object', () => {
    const config = ConfigSchema.parse({});
    assert.equal(config.device.port, 27_100);
    assert.equal(config.device.backoff.maxMs, 5000);
    assert.equal(config.audio.drift.maxPpm, 1000);
    assert.equal(config.usbmux.address, '127.0.0.1:27015');
    assert.deepEqual(
      [config.camera.width, config.camera.height, config.camera.fps],
      [1920, 1080, 30],
      'install default mode',
    );
    assert.deepEqual([config.camera.maxWidth, config.camera.maxHeight, config.camera.maxFps], [3840, 2160, 60]);
    assert.equal(config.video.bitrateKbps, undefined, 'bitrate follows the mode unless overridden');
    assert.equal(config.video.bitsPerPixel, 0.1);
    assert.equal(config.video.modeDowngradeGraceMs, 3000);
    assert.equal(config.video.hwaccel, 'auto');
  });

  it('accepts a bitrate override up to the protocol maximum', () => {
    assert.equal(ConfigSchema.parse({ video: { bitrateKbps: 40_000 } }).video.bitrateKbps, 40_000);
    assert.equal(ConfigSchema.safeParse({ video: { bitrateKbps: 40_001 } }).success, false);
    assert.equal(ConfigSchema.safeParse({ video: { bitrateKbps: 499 } }).success, false);
  });

  it('rejects unknown keys so typos surface', () => {
    assert.equal(ConfigSchema.safeParse({ video: { bitrate: 1 } }).success, false);
  });
});

describe('ConfigLoader', () => {
  it('merges file values and CLI overrides over defaults', () => {
    const dir = mkdtempSync(join(tmpdir(), 'ipb-config-'));
    writeFileSync(
      join(dir, 'bridge.config.json'),
      JSON.stringify({ video: { bitrateKbps: 8000 }, device: { port: 1234 } }),
    );
    const { config, source } = new ConfigLoader({ cwd: dir, appData: undefined }).load(undefined, {
      device: { id: 'abc' },
    });
    assert.equal(source, join(dir, 'bridge.config.json'));
    assert.equal(config.video.bitrateKbps, 8000);
    assert.equal(config.device.port, 1234);
    assert.equal(config.device.id, 'abc');
    assert.equal(config.device.pingIntervalMs, 1000);
  });

  it('reports invalid files as ConfigError', () => {
    const dir = mkdtempSync(join(tmpdir(), 'ipb-config-'));
    writeFileSync(join(dir, 'bridge.config.json'), '{"video":{"bitrateKbps":"fast"}}');
    assert.throws(() => new ConfigLoader({ cwd: dir, appData: undefined }).load(undefined), ConfigError);
  });
});

describe('buildDecoderArgs', () => {
  it('produces the low-latency decoder command line', () => {
    const args = buildDecoderArgs({
      mode: { width: 1280, height: 720 },
      output: String.raw`\\.\pipe\x`,
      hwaccel: 'none',
    });
    assert.ok(args.includes('passthrough'), 'fps_mode passthrough');
    assert.ok(!args.includes('nobuffer'), 'nobuffer drops frames with raw H.264');
    assert.equal(args.at(-1), String.raw`\\.\pipe\x`);
    const filter = args[args.indexOf('-vf') + 1] ?? '';
    assert.match(filter, /^scale=1280:720:.*,pad=1280:720:.*,format=nv12$/);
  });

  it('adds hardware decoding only when requested', () => {
    assert.ok(
      !buildDecoderArgs({ mode: { width: 640, height: 360 }, output: 'o', hwaccel: 'none' }).includes('-hwaccel'),
    );
    const args = buildDecoderArgs({ mode: { width: 640, height: 360 }, output: 'o', hwaccel: 'd3d11va' });
    assert.equal(args[args.indexOf('-hwaccel') + 1], 'd3d11va');
  });

  it('decodes to the given mode size', () => {
    const args = buildDecoderArgs({ mode: { width: 3840, height: 2160 }, output: 'o', hwaccel: 'd3d11va' });
    assert.match(args[args.indexOf('-vf') + 1] ?? '', /^scale=3840:2160:.*,pad=3840:2160:/);
  });
});

describe('resolveHardwareAcceleration', () => {
  it('auto uses D3D11VA above 1080p and software up to 1080p', () => {
    assert.equal(resolveHardwareAcceleration('auto', { width: 3840, height: 2160 }), 'd3d11va');
    assert.equal(resolveHardwareAcceleration('auto', { width: 2560, height: 1440 }), 'd3d11va');
    assert.equal(resolveHardwareAcceleration('auto', { width: 1920, height: 1080 }), 'none');
    assert.equal(resolveHardwareAcceleration('auto', { width: 640, height: 360 }), 'none');
  });

  it('keeps an explicit choice for every mode', () => {
    assert.equal(resolveHardwareAcceleration('none', { width: 3840, height: 2160 }), 'none');
    assert.equal(resolveHardwareAcceleration('dxva2', { width: 640, height: 360 }), 'dxva2');
  });
});

describe('maxBacklogBytesFor', () => {
  it('holds a whole 4K IDR above 1080p and stays at 1 MiB up to 1080p', () => {
    assert.equal(maxBacklogBytesFor({ width: 1920, height: 1080 }), 1024 * 1024);
    assert.equal(maxBacklogBytesFor({ width: 640, height: 480 }), 1024 * 1024);
    assert.equal(maxBacklogBytesFor({ width: 2560, height: 1440 }), 8 * 1024 * 1024);
    assert.equal(maxBacklogBytesFor({ width: 3840, height: 2160 }), 8 * 1024 * 1024);
  });
});

describe('bridge-native output schemas (protocol/BRIDGE_NATIVE.md examples)', () => {
  const status = {
    ok: true,
    installDir: 'C:\\Program Files\\MobileWebcamBridge\\0.1.0',
    os: { build: 26200, isWin11: true },
    camera: {
      installed: true,
      backend: 'mf',
      friendlyName: 'Mobile Webcam',
      width: 1920,
      height: 1080,
      fpsNum: 30,
      fpsDen: 1,
      maxWidth: 3840,
      maxHeight: 2160,
      maxFps: 60,
      modes: [
        { width: 1920, height: 1080, fpsNum: 30, fpsDen: 1 },
        { width: 3840, height: 2160, fpsNum: 30, fpsDen: 1 },
      ],
      pipeName: 'mobile-webcam-bridge-video',
    },
    mic: { installed: true, devicePresent: true, problemCode: 0 },
  };

  it('accepts the documented status object', () => {
    const parsed = StatusSchema.parse(status);
    assert.ok(parsed.camera.installed);
    assert.equal(parsed.camera.maxFps, 60);
    assert.equal(parsed.camera.modes.length, 2);
  });

  it('requires the cap and the mode list of an installed camera', () => {
    const { maxFps: _maxFps, ...withoutCap } = status.camera;
    assert.equal(StatusSchema.safeParse({ ...status, camera: withoutCap }).success, false);
    const { modes: _modes, ...withoutModes } = status.camera;
    assert.equal(StatusSchema.safeParse({ ...status, camera: withoutModes }).success, false);
    assert.equal(StatusSchema.safeParse({ ...status, camera: { installed: false } }).success, true);
  });

  it('accepts the documented hub ready event with the cap', () => {
    const ready = {
      event: 'ready',
      ingestPipe: '\\\\.\\pipe\\mobile-webcam-bridge-ingest-0123456789abcdef',
      publicPipe: '\\\\.\\pipe\\mobile-webcam-bridge-video',
      width: 1920,
      height: 1080,
      fpsNum: 30,
      fpsDen: 1,
      maxWidth: 3840,
      maxHeight: 2160,
      maxFps: 60,
    };
    assert.equal(HubEventSchema.parse(ready).event, 'ready');
    const { maxWidth: _maxWidth, ...withoutCap } = ready;
    assert.equal(HubEventSchema.safeParse(withoutCap).success, false);
  });

  it('accepts consumers events with one catalog mode per consumer', () => {
    const modes = [
      { width: 1920, height: 1080, fpsNum: 30, fpsDen: 1 },
      { width: 640, height: 480, fpsNum: 30, fpsDen: 1 },
    ];
    const parsed = HubEventSchema.parse({ event: 'consumers', count: 2, modes });
    assert.deepEqual(parsed.event === 'consumers' && parsed.modes, modes);
    assert.equal(HubEventSchema.safeParse({ event: 'consumers', count: 0, modes: [] }).success, true);
    assert.equal(HubEventSchema.safeParse({ event: 'consumers', count: 3, modes }).success, false, 'count mismatch');
    assert.equal(HubEventSchema.safeParse({ event: 'consumers', count: 1 }).success, false, 'modes required');
    assert.equal(
      HubEventSchema.safeParse({
        event: 'consumers',
        count: 1,
        modes: [{ width: 3840, height: 2160, fpsNum: 60, fpsDen: 1 }],
      }).success,
      false,
      '4K60 is not a catalog mode',
    );
  });

  it('accepts the ingestMode acknowledgement', () => {
    assert.deepEqual(HubEventSchema.parse({ event: 'ingestMode', width: 3840, height: 2160 }), {
      event: 'ingestMode',
      width: 3840,
      height: 2160,
    });
    assert.equal(HubEventSchema.safeParse({ event: 'ingestMode', width: 0, height: 2160 }).success, false);
  });

  it('accepts the documented mic events', () => {
    assert.equal(
      MicEventSchema.parse({
        event: 'status',
        bufferedBytes: 3840,
        capacityBytes: 65536,
        streamActive: true,
        underruns: 0,
        overruns: 0,
      }).event,
      'status',
    );
  });
});

describe('InMemoryMetrics', () => {
  it('drains counters and distributions but keeps gauges', () => {
    const metrics = new InMemoryMetrics();
    metrics.increment('a', 2);
    metrics.gauge('g', 5);
    for (let i = 1; i <= 100; i++) metrics.observe('d', i);
    const first = metrics.drain();
    assert.equal(first.counters['a'], 2);
    assert.deepEqual(first.distributions['d'], { count: 100, p50: 50, p95: 95, max: 100 });
    const second = metrics.drain();
    assert.equal(second.counters['a'], undefined);
    assert.equal(second.gauges['g'], 5);
  });
});

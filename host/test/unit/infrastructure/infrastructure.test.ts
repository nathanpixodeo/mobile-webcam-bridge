import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, it } from 'node:test';
import { ConfigError } from '#domain/errors.ts';
import { ConfigLoader } from '#infrastructure/config/ConfigLoader.ts';
import { ConfigSchema } from '#infrastructure/config/ConfigSchema.ts';
import { buildDecoderArgs } from '#infrastructure/ffmpeg/FfmpegArgsBuilder.ts';
import { HubEventSchema, MicEventSchema, StatusSchema } from '#infrastructure/native/BridgeNativeSchemas.ts';
import { InMemoryMetrics } from '#infrastructure/metrics/InMemoryMetrics.ts';

describe('ConfigSchema', () => {
  it('fills every nested default from an empty object', () => {
    const config = ConfigSchema.parse({});
    assert.equal(config.device.port, 27_100);
    assert.equal(config.device.backoff.maxMs, 5000);
    assert.equal(config.audio.drift.maxPpm, 1000);
    assert.equal(config.usbmux.address, '127.0.0.1:27015');
    assert.equal(config.camera.width, 1280);
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
});

describe('bridge-native output schemas (protocol/BRIDGE_NATIVE.md examples)', () => {
  it('accepts the documented status object', () => {
    const parsed = StatusSchema.parse({
      ok: true,
      installDir: 'C:\\Program Files\\MobileWebcamBridge\\0.1.0',
      os: { build: 26200, isWin11: true },
      camera: {
        installed: true,
        backend: 'mf',
        friendlyName: 'iPhone',
        width: 1280,
        height: 720,
        fpsNum: 30,
        fpsDen: 1,
        pipeName: 'mobile-webcam-bridge-video',
      },
      mic: { installed: true, devicePresent: true, problemCode: 0 },
    });
    assert.equal(parsed.camera.installed, true);
  });

  it('accepts the documented hub and mic events', () => {
    assert.equal(HubEventSchema.parse({ event: 'consumers', count: 1 }).event, 'consumers');
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

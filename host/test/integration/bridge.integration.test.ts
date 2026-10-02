import assert from 'node:assert/strict';
import { before, describe, it } from 'node:test';
import { setTimeout as sleep } from 'node:timers/promises';
import { createBridgeApp } from '#composition/createBridgeApp.ts';
import type { StartVideo } from '#domain/protocol/messages.ts';
import { PacketType } from '#domain/protocol/PacketType.ts';
import type { VideoMode } from '#domain/video/VideoMode.ts';
import { silentLogger } from '#ports/Logger.ts';
import { ConfigSchema } from '#infrastructure/config/ConfigSchema.ts';
import { SystemClock } from '#infrastructure/system/SystemClock.ts';
import { StaticDeviceWatcher } from '#infrastructure/tcp/StaticDeviceWatcher.ts';
import { TcpTunnelFactory } from '#infrastructure/tcp/TcpTunnelFactory.ts';
import { FakeCompanion } from '#test/fakes/FakeCompanion.ts';
import { FakeAudioSink, FakeVideoOutput } from '#test/fakes/FakeMedia.ts';
import { ffmpegAvailable, h264Fixture, splitAccessUnits } from '#test/support/h264Fixture.ts';
import { waitFor } from '#test/support/waitFor.ts';

const SMALL: VideoMode = { width: 640, height: 360, fpsNum: 30, fpsDen: 1 };
const HD: VideoMode = { width: 1280, height: 720, fpsNum: 30, fpsDen: 1 };

describe(
  'Bridge end to end (fake device, fake virtual devices, real ffmpeg)',
  { skip: !ffmpegAvailable() && 'ffmpeg with libx264 required' },
  () => {
    let accessUnits: Buffer[] = [];

    before(() => {
      accessUnits = splitAccessUnits(h264Fixture({ width: 320, height: 240, fps: 30, seconds: 2 }));
    });

    it('streams on demand: camera consumers drive video, microphone capture drives audio', async () => {
      await using companion = await FakeCompanion.start({ accessUnits, fps: 30 });
      const output = await FakeVideoOutput.start(SMALL);
      const sink = new FakeAudioSink();
      const watcher = new StaticDeviceWatcher();
      const config = ConfigSchema.parse({
        device: { port: companion.port, pingIntervalMs: 100, heartbeatTimeoutMs: 1000 },
        video: { stopGraceMs: 300 },
        audio: { stopGraceMs: 300 },
        logging: { metricsIntervalMs: 60_000 },
      });
      const app = await createBridgeApp(
        { config, logger: silentLogger, clock: new SystemClock() },
        {
          videoOutput: output,
          audioSink: sink,
          devices: { watcher, tunnels: new TcpTunnelFactory(), usbmux: undefined, adb: undefined },
        },
      );
      const abort = new AbortController();
      const running = app.run(abort.signal);
      try {
        await waitFor(() => companion.received(PacketType.Hello).length === 1, 'host Hello');
        await waitFor(() => output.placeholders.at(-1) === null, 'live placeholder state once connected');
        assert.equal(companion.videoRunning, false, 'no video without consumers');

        output.setConsumers([SMALL]);
        await waitFor(() => companion.videoRunning, 'video started on demand');
        await waitFor(() => output.frames >= 15, 'decoded frames reach the camera', 30_000);

        output.setConsumers([]);
        await waitFor(() => !companion.videoRunning, 'video stopped after the grace period');

        sink.emitStatus({ bufferedBytes: 0, capacityBytes: 65_536, streamActive: true, underruns: 0, overruns: 0 });
        await waitFor(() => companion.audioRunning, 'audio started when the microphone is captured');
        await waitFor(() => sink.samplesWritten >= 48_000 * 0.2, '200 ms of audio delivered');
        sink.emitStatus({ bufferedBytes: 0, capacityBytes: 65_536, streamActive: false, underruns: 0, overruns: 0 });
        await waitFor(() => !companion.audioRunning, 'audio stopped after the grace period');

        watcher.detach();
        await waitFor(() => output.placeholders.at(-1) === 'no-device', 'no-device placeholder after unplug');
      } finally {
        abort.abort();
        await running;
        await app[Symbol.asyncDispose]();
      }
    });

    it('streams the mode the consumers need: larger at once, smaller after the grace period', async () => {
      await using companion = await FakeCompanion.start({ accessUnits, fps: 30 });
      const output = await FakeVideoOutput.start(HD);
      const watcher = new StaticDeviceWatcher();
      const config = ConfigSchema.parse({
        device: { port: companion.port, pingIntervalMs: 100, heartbeatTimeoutMs: 1000 },
        video: { stopGraceMs: 300, modeDowngradeGraceMs: 500 },
        logging: { metricsIntervalMs: 60_000 },
      });
      const app = await createBridgeApp(
        { config, logger: silentLogger, clock: new SystemClock() },
        {
          videoOutput: output,
          audioSink: null,
          devices: { watcher, tunnels: new TcpTunnelFactory(), usbmux: undefined, adb: undefined },
        },
      );
      // Read through a call: assertions would otherwise narrow the getter for the rest of the test.
      const requested = (): StartVideo | undefined => companion.video;
      const abort = new AbortController();
      const running = app.run(abort.signal);
      try {
        await waitFor(() => companion.received(PacketType.Hello).length === 1, 'host Hello');

        output.setConsumers([SMALL]);
        await waitFor(() => companion.videoRunning, 'video started on demand');
        assert.equal(requested()?.width, 640);
        assert.equal(requested()?.height, 360);
        assert.equal(requested()?.bitrateKbps, 1500, 'derived bitrate, clamped to the minimum');
        await waitFor(
          () => output.ingestMode.width === 640 && output.frames >= 10,
          'frames at 640x360 (the first stream skips the 1280x720 default)',
          30_000,
        );

        output.setConsumers([SMALL, HD]);
        await waitFor(() => requested()?.width === 1280, 'StartVideo for 1280x720');
        assert.equal(requested()?.bitrateKbps, 2765);
        await waitFor(() => output.ingestMode.width === 1280 && output.frames >= 10, 'frames at 1280x720', 30_000);

        output.setConsumers([SMALL]);
        await sleep(200);
        assert.equal(requested()?.width, 1280, 'the larger mode is kept during the grace period');
        await waitFor(() => requested()?.width === 640, 'StartVideo for 640x360 after the grace period');
        await waitFor(() => output.ingestMode.width === 640 && output.frames >= 10, 'frames at 640x360', 30_000);
        assert.equal(companion.received(PacketType.StartVideo).length, 3);
        assert.deepEqual(output.ingestRequests, [
          { width: 640, height: 360 },
          { width: 1280, height: 720 },
          { width: 640, height: 360 },
        ]);
      } finally {
        abort.abort();
        await running;
        await app[Symbol.asyncDispose]();
      }
    });
  },
);

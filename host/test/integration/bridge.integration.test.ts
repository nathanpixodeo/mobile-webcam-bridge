import assert from 'node:assert/strict';
import { before, describe, it } from 'node:test';
import { createBridgeApp } from '#composition/createBridgeApp.ts';
import { PacketType } from '#domain/protocol/PacketType.ts';
import { silentLogger } from '#ports/Logger.ts';
import { ConfigSchema } from '#infrastructure/config/ConfigSchema.ts';
import { SystemClock } from '#infrastructure/system/SystemClock.ts';
import { StaticDeviceWatcher } from '#infrastructure/tcp/StaticDeviceWatcher.ts';
import { TcpTunnelFactory } from '#infrastructure/tcp/TcpTunnelFactory.ts';
import { FakeCompanion } from '#test/fakes/FakeCompanion.ts';
import { FakeAudioSink, FakeVideoOutput } from '#test/fakes/FakeMedia.ts';
import { ffmpegAvailable, h264Fixture, splitAccessUnits } from '#test/support/h264Fixture.ts';
import { waitFor } from '#test/support/waitFor.ts';

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
      const output = await FakeVideoOutput.start({ width: 320, height: 240, fpsNum: 30, fpsDen: 1 });
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

        output.setConsumers(1);
        await waitFor(() => companion.videoRunning, 'video started on demand');
        await waitFor(() => output.frames >= 15, 'decoded frames reach the camera', 30_000);

        output.setConsumers(0);
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
  },
);

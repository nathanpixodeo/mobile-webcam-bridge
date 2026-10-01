import assert from 'node:assert/strict';
import { createServer } from 'node:net';
import { after, before, describe, it } from 'node:test';
import { DeviceSession, type SessionState } from '#application/DeviceSession.ts';
import { PacketType } from '#domain/protocol/PacketType.ts';
import { ReconnectPolicy } from '#domain/session/ReconnectPolicy.ts';
import { silentLogger } from '#ports/Logger.ts';
import { SystemClock } from '#infrastructure/system/SystemClock.ts';
import { TcpTunnelFactory } from '#infrastructure/tcp/TcpTunnelFactory.ts';
import { FakeCompanion } from '#test/fakes/FakeCompanion.ts';
import { ffmpegAvailable, h264Fixture, splitAccessUnits } from '#test/support/h264Fixture.ts';
import { waitFor } from '#test/support/waitFor.ts';

const VIDEO = {
  width: 320,
  height: 240,
  fps: 30,
  bitrateKbps: 1000,
  camera: 'back.wide',
  mirror: false,
  orientation: 'auto',
  encoder: 'lowLatency',
} as const;

function createSession(port: number): DeviceSession {
  return new DeviceSession({
    device: { transport: 'tcp', platform: 'unknown', id: 'fake', link: 'network', model: undefined },
    port,
    tunnels: new TcpTunnelFactory(),
    policy: new ReconnectPolicy({
      backoff: { initialMs: 50, maxMs: 200, factor: 2, jitter: 0 },
      appPollMs: 100,
      stableAfterMs: 1000,
    }),
    timings: { connectTimeoutMs: 1000, handshakeTimeoutMs: 1000, pingIntervalMs: 100, heartbeatTimeoutMs: 500 },
    hello: { app: { name: 'test-host', version: '0.0.0', build: 'test', gitSha: 'test' }, features: [] },
    clock: new SystemClock(),
    logger: silentLogger,
  });
}

async function freePort(): Promise<number> {
  const server = createServer();
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const address = server.address();
  await new Promise<void>((resolve) => {
    server.close(() => {
      resolve();
    });
  });
  if (address === null || typeof address === 'string') throw new Error('no port');
  return address.port;
}

describe(
  'DeviceSession with a fake companion over TCP',
  { skip: !ffmpegAvailable() && 'ffmpeg with libx264 required' },
  () => {
    let accessUnits: Buffer[] = [];

    before(() => {
      accessUnits = splitAccessUnits(h264Fixture({ width: 320, height: 240, fps: 30, seconds: 2 }));
    });

    it('handshakes, requests the desired streams and receives media', async () => {
      await using companion = await FakeCompanion.start({ accessUnits, fps: 30 });
      await using session = createSession(companion.port);
      const states: SessionState['kind'][] = [];
      let received = 0;
      let idrSeen = false;
      session.events.on('state', (state) => states.push(state.kind));
      session.events.on('accessUnit', (accessUnit) => {
        received++;
        idrSeen ||= accessUnit.isIdr;
      });
      session.setDesiredStreams({ video: VIDEO, audio: null });
      session.start();

      await waitFor(() => received >= 10, '10 access units');
      assert.ok(idrSeen);
      assert.deepEqual(states.slice(0, 3), ['connecting', 'handshaking', 'ready']);
      assert.equal(companion.received(PacketType.Hello).length, 1);
      assert.equal(companion.received(PacketType.StartVideo).length, 1);
      await waitFor(() => session.timing.rttUs !== undefined, 'an RTT sample');
    });

    it('reconnects after a heartbeat timeout and replays the desired streams', async () => {
      await using companion = await FakeCompanion.start({ accessUnits, fps: 30 });
      await using session = createSession(companion.port);
      session.setDesiredStreams({ video: VIDEO, audio: { processing: 'standard' } });
      session.start();
      await waitFor(() => companion.videoRunning && companion.audioRunning, 'streams running');

      companion.goSilent();
      await waitFor(() => companion.connections >= 2, 'a second connection', 4000);
      await waitFor(() => companion.videoRunning && companion.audioRunning, 'streams replayed after reconnect');
      assert.equal(companion.received(PacketType.StartVideo).length, 2);
      assert.equal(companion.received(PacketType.StartAudio).length, 2);
    });

    it('polls while the app is not listening and connects once it is', async () => {
      const port = await freePort();
      await using session = createSession(port);
      session.start();
      await waitFor(
        () => session.state.kind === 'backoff' && session.state.reason.code === 'APP_NOT_REACHABLE',
        'APP_NOT_REACHABLE backoff',
      );
      await using _companion = await FakeCompanion.start({ accessUnits, fps: 30, port });
      await waitFor(() => session.state.kind === 'ready', 'ready after the app starts');
    });

    it('stops on a protocol major version mismatch', async () => {
      await using companion = await FakeCompanion.start({ accessUnits, fps: 30, protocolMajor: 2 });
      await using session = createSession(companion.port);
      session.start();
      await waitFor(() => session.state.kind === 'stalled', 'stalled session');
      assert.equal(session.state.kind === 'stalled' && session.state.reason.code, 'VERSION_MISMATCH');
      assert.equal(companion.received(PacketType.Error).length, 1);
    });

    it('sends Stop commands when disposed while streaming', async () => {
      await using companion = await FakeCompanion.start({ accessUnits, fps: 30 });
      const session = createSession(companion.port);
      session.setDesiredStreams({ video: VIDEO, audio: null });
      session.start();
      await waitFor(() => companion.videoRunning, 'video running');
      await session[Symbol.asyncDispose]();
      await waitFor(() => companion.received(PacketType.StopVideo).length === 1, 'StopVideo received');
    });

    after(() => undefined);
  },
);

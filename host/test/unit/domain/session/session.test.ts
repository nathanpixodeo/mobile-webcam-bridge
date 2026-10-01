import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { ProtocolError, RemoteError, TransportError } from '#domain/errors.ts';
import { ClockOffsetEstimator } from '#domain/session/ClockOffsetEstimator.ts';
import { ExponentialBackoff } from '#domain/session/ExponentialBackoff.ts';
import { ReconnectPolicy } from '#domain/session/ReconnectPolicy.ts';
import { NO_STREAMS, StreamReconciler } from '#domain/session/StreamReconciler.ts';
import type { StartVideo } from '#domain/protocol/messages.ts';

describe('ExponentialBackoff', () => {
  it('grows by the factor up to the cap without jitter', () => {
    const backoff = new ExponentialBackoff({ initialMs: 250, maxMs: 5000, factor: 2, jitter: 0 });
    assert.deepEqual(
      Array.from({ length: 7 }, () => backoff.next()),
      [250, 500, 1000, 2000, 4000, 5000, 5000],
    );
    backoff.reset();
    assert.equal(backoff.next(), 250);
  });

  it('applies symmetric jitter from the injected random source', () => {
    const low = new ExponentialBackoff({ initialMs: 1000, maxMs: 5000, factor: 2, jitter: 0.2, random: () => 0 });
    const high = new ExponentialBackoff({
      initialMs: 1000,
      maxMs: 5000,
      factor: 2,
      jitter: 0.2,
      random: () => 0.999999,
    });
    assert.equal(low.next(), 800);
    assert.equal(high.next(), 1200);
  });

  it('rejects nonsensical options', () => {
    assert.throws(() => new ExponentialBackoff({ initialMs: 0, maxMs: 10, factor: 2, jitter: 0 }), RangeError);
  });
});

describe('ReconnectPolicy', () => {
  const policy = (): ReconnectPolicy =>
    new ReconnectPolicy({
      backoff: { initialMs: 250, maxMs: 5000, factor: 2, jitter: 0 },
      appPollMs: 1000,
      stableAfterMs: 10_000,
    });

  it('polls quickly and quietly while the iPhone app is closed', () => {
    const p = policy();
    const refused = new TransportError('APP_NOT_REACHABLE', 'refused');
    assert.deepEqual(p.decide(refused), { kind: 'retry', delayMs: 1000, quiet: false });
    assert.deepEqual(p.decide(refused), { kind: 'retry', delayMs: 1000, quiet: true });
  });

  it('waits for re-attach when the device disappears', () => {
    assert.deepEqual(policy().decide(new TransportError('DEVICE_UNAVAILABLE', 'gone')), { kind: 'waitForReattach' });
  });

  it('gives up on a version mismatch', () => {
    assert.deepEqual(policy().decide(ProtocolError.versionMismatch(1, 2)), { kind: 'giveUp' });
  });

  it('backs off exponentially on connection loss and resets after a stable connection', () => {
    const p = policy();
    const lost = new TransportError('HEARTBEAT_TIMEOUT', 'silent');
    assert.deepEqual(p.decide(lost), { kind: 'retry', delayMs: 250, quiet: false });
    assert.deepEqual(p.decide(lost), { kind: 'retry', delayMs: 500, quiet: false });
    p.onDisconnected(9_000);
    assert.deepEqual(p.decide(lost), { kind: 'retry', delayMs: 1000, quiet: false });
    p.onDisconnected(10_000);
    assert.deepEqual(p.decide(lost), { kind: 'retry', delayMs: 250, quiet: false });
  });

  it('retries device-reported errors', () => {
    assert.equal(policy().decide(new RemoteError('PERMISSION_DENIED', 'no camera', true)).kind, 'retry');
  });
});

describe('ClockOffsetEstimator', () => {
  it('computes NTP offset and RTT from the minimum-RTT sample', () => {
    const estimator = new ClockOffsetEstimator(4);
    assert.equal(estimator.offsetUs, undefined);
    // Peer clock = local + 5000 µs; symmetric 1 ms path, 100 µs processing.
    estimator.add({ t0: 0n, t1: 6_000n, t2: 6_100n, t3: 2_100n });
    // A congested exchange (asymmetric) that must not win.
    estimator.add({ t0: 10_000n, t1: 25_000n, t2: 25_100n, t3: 21_100n });
    assert.equal(estimator.rttUs, 2_000n);
    assert.equal(estimator.offsetUs, 5_000n);
    assert.equal(estimator.toLocal(105_000n), 100_000n);
  });

  it('ignores impossible samples', () => {
    const estimator = new ClockOffsetEstimator();
    estimator.add({ t0: 100n, t1: 0n, t2: 1_000n, t3: 50n });
    assert.equal(estimator.offsetUs, undefined);
  });
});

describe('StreamReconciler', () => {
  const video: StartVideo = {
    width: 1280,
    height: 720,
    fps: 30,
    bitrateKbps: 6000,
    camera: 'back.wide',
    mirror: false,
    orientation: 'auto',
    encoder: 'lowLatency',
  };

  it('starts, reconfigures and stops streams level-triggered', () => {
    const reconciler = new StreamReconciler();
    assert.deepEqual(reconciler.reconcile({ video, audio: null }), [{ kind: 'startVideo', params: video }]);
    assert.deepEqual(reconciler.reconcile({ video, audio: null }), [], 'idempotent');
    const faster = { ...video, fps: 60 };
    assert.deepEqual(reconciler.reconcile({ video: faster, audio: { processing: 'raw' } }), [
      { kind: 'startVideo', params: faster },
      { kind: 'startAudio', params: { processing: 'raw' } },
    ]);
    assert.deepEqual(reconciler.reconcile(NO_STREAMS), [{ kind: 'stopVideo' }, { kind: 'stopAudio' }]);
  });

  it('replays desired streams after reset (reconnect)', () => {
    const reconciler = new StreamReconciler();
    reconciler.reconcile({ video, audio: null });
    reconciler.reset();
    assert.deepEqual(reconciler.reconcile({ video, audio: null }), [{ kind: 'startVideo', params: video }]);
  });
});

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { StreamCoordinator } from '#application/StreamCoordinator.ts';
import type { StartVideo } from '#domain/protocol/messages.ts';
import { StreamReconciler, type DesiredStreams } from '#domain/session/StreamReconciler.ts';

const VIDEO: StartVideo = {
  width: 1920,
  height: 1080,
  fps: 30,
  bitrateKbps: 6221,
  camera: 'back.wide',
  mirror: false,
  orientation: 'auto',
  encoder: 'lowLatency',
};

const setup = (): { coordinator: StreamCoordinator; changes: DesiredStreams[] } => {
  const changes: DesiredStreams[] = [];
  const coordinator = new StreamCoordinator({
    video: VIDEO,
    audio: { processing: 'standard' },
    onChange: (desired) => changes.push(desired),
  });
  return { coordinator, changes };
};

describe('StreamCoordinator', () => {
  it('asks for video and audio while each has demand', () => {
    const { coordinator, changes } = setup();
    coordinator.setVideoDemand(true);
    coordinator.setAudioDemand(true);
    coordinator.setVideoDemand(false);
    assert.deepEqual(changes, [
      { video: VIDEO, audio: null },
      { video: VIDEO, audio: { processing: 'standard' } },
      { video: null, audio: { processing: 'standard' } },
    ]);
  });

  it('reconfigures running video with new parameters, which yields a new StartVideo', () => {
    const { coordinator, changes } = setup();
    const reconciler = new StreamReconciler();
    coordinator.setVideoDemand(true);
    assert.deepEqual(reconciler.reconcile(coordinator.desired), [{ kind: 'startVideo', params: VIDEO }]);

    const uhd = { ...VIDEO, width: 3840, height: 2160, bitrateKbps: 24_883 };
    coordinator.setVideoParams(uhd);
    assert.equal(changes.length, 2);
    assert.deepEqual(changes.at(-1), { video: uhd, audio: null });
    assert.deepEqual(reconciler.reconcile(coordinator.desired), [{ kind: 'startVideo', params: uhd }]);
  });

  it('only remembers new parameters while video is not wanted', () => {
    const { coordinator, changes } = setup();
    const hd = { ...VIDEO, width: 1280, height: 720 };
    coordinator.setVideoParams(hd);
    assert.deepEqual(changes, []);
    coordinator.setVideoDemand(true);
    assert.deepEqual(changes, [{ video: hd, audio: null }]);
  });

  it('ignores parameters equal to the current ones', () => {
    const { coordinator, changes } = setup();
    coordinator.setVideoDemand(true);
    coordinator.setVideoParams({ ...VIDEO });
    assert.equal(changes.length, 1);
  });
});

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { DemandTracker } from '#domain/demand/DemandTracker.ts';
import { FakeClock } from '#test/fakes/FakeClock.ts';

describe('DemandTracker', () => {
  const setup = (): { clock: FakeClock; changes: boolean[]; tracker: DemandTracker } => {
    const clock = new FakeClock();
    const changes: boolean[] = [];
    const tracker = new DemandTracker({ stopGraceMs: 3000, clock, onChange: (active) => changes.push(active) });
    return { clock, changes, tracker };
  };

  it('reports rising demand immediately and falling demand after the grace period', async () => {
    const { clock, changes, tracker } = setup();
    tracker.update(1);
    assert.deepEqual(changes, [true]);
    tracker.update(0);
    await clock.advance(2999);
    assert.deepEqual(changes, [true]);
    await clock.advance(1);
    assert.deepEqual(changes, [true, false]);
    assert.equal(tracker.active, false);
  });

  it('cancels a pending stop when demand returns within the grace period', async () => {
    const { clock, changes, tracker } = setup();
    tracker.update(2);
    tracker.update(0);
    await clock.advance(1500);
    tracker.update(1);
    await clock.advance(5000);
    assert.deepEqual(changes, [true]);
  });

  it('does not report repeated values', async () => {
    const { clock, changes, tracker } = setup();
    tracker.update(0);
    await clock.advance(10_000);
    tracker.update(1);
    tracker.update(3);
    assert.deepEqual(changes, [true]);
  });

  it('cancels timers on dispose', async () => {
    const { clock, changes, tracker } = setup();
    tracker.update(1);
    tracker.update(0);
    tracker[Symbol.dispose]();
    await clock.advance(10_000);
    assert.deepEqual(changes, [true]);
    assert.equal(clock.pendingTimers, 0);
  });
});

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { bitrateKbpsFor } from '#domain/video/BitratePolicy.ts';
import { ModeArbiter } from '#domain/video/ModeArbiter.ts';
import { CATALOG_FRAME_RATES, CATALOG_SIZES, isCatalogMode, maxFpsFor } from '#domain/video/ModeCatalog.ts';
import { compareModes, sameMode, type VideoMode } from '#domain/video/VideoMode.ts';
import { FakeClock } from '#test/fakes/FakeClock.ts';

const mode = (width: number, height: number, fps: number): VideoMode => ({ width, height, fpsNum: fps, fpsDen: 1 });

describe('ModeCatalog', () => {
  it('lists the protocol/FRAME_PIPE.md §1 sizes and frame rates', () => {
    assert.deepEqual(
      CATALOG_SIZES.map(({ width, height }) => `${width}x${height}`),
      ['640x360', '640x480', '960x540', '1280x720', '1920x1080', '2560x1440', '3840x2160'],
    );
    assert.deepEqual(CATALOG_FRAME_RATES, [15, 30, 60]);
  });

  it('offers 60 fps up to 1440p and 30 fps at 4K', () => {
    assert.equal(maxFpsFor({ width: 640, height: 360 }), 60);
    assert.equal(maxFpsFor({ width: 2560, height: 1440 }), 60);
    assert.equal(maxFpsFor({ width: 3840, height: 2160 }), 30);
    assert.equal(maxFpsFor({ width: 800, height: 600 }), undefined);
  });

  it('recognises catalog modes only', () => {
    assert.ok(isCatalogMode(mode(1920, 1080, 60)));
    assert.ok(isCatalogMode(mode(3840, 2160, 15)));
    assert.ok(isCatalogMode(mode(3840, 2160, 30)));
    assert.ok(!isCatalogMode(mode(3840, 2160, 60)), '4K60 is not offered');
    assert.ok(!isCatalogMode(mode(1280, 720, 25)), 'not a catalog frame rate');
    assert.ok(!isCatalogMode(mode(320, 240, 30)), 'not a catalog size');
    assert.ok(!isCatalogMode({ width: 1280, height: 720, fpsNum: 30_000, fpsDen: 1001 }), 'integer rates only');
  });
});

describe('compareModes', () => {
  it('orders by area, then by frame rate', () => {
    assert.ok(compareModes(mode(1920, 1080, 15), mode(1280, 720, 60)) > 0);
    assert.ok(compareModes(mode(1280, 720, 30), mode(1280, 720, 60)) < 0);
    assert.equal(compareModes(mode(1280, 720, 30), mode(1280, 720, 30)), 0);
  });

  it('treats equal frame rates with different fractions as the same mode', () => {
    assert.ok(sameMode(mode(1280, 720, 30), { width: 1280, height: 720, fpsNum: 60, fpsDen: 2 }));
    assert.ok(!sameMode(mode(1280, 720, 30), mode(1280, 720, 60)));
  });
});

describe('bitrateKbpsFor', () => {
  it('scales with the pixel rate', () => {
    assert.equal(bitrateKbpsFor(mode(1920, 1080, 30), { bitsPerPixel: 0.1 }), 6221);
    assert.equal(bitrateKbpsFor(mode(1920, 1080, 60), { bitsPerPixel: 0.1 }), 12_442);
    assert.equal(bitrateKbpsFor(mode(3840, 2160, 30), { bitsPerPixel: 0.1 }), 24_883);
  });

  it('clamps to 1500..40000 kbps', () => {
    assert.equal(bitrateKbpsFor(mode(640, 360, 15), { bitsPerPixel: 0.1 }), 1500);
    assert.equal(bitrateKbpsFor(mode(3840, 2160, 30), { bitsPerPixel: 0.2 }), 40_000);
  });

  it('uses the configured override for every mode', () => {
    assert.equal(bitrateKbpsFor(mode(640, 360, 15), { bitsPerPixel: 0.1, overrideKbps: 800 }), 800);
    assert.equal(bitrateKbpsFor(mode(3840, 2160, 30), { bitsPerPixel: 0.1, overrideKbps: 8000 }), 8000);
    assert.equal(bitrateKbpsFor(mode(1920, 1080, 30), { bitsPerPixel: 0.1, overrideKbps: undefined }), 6221);
  });
});

describe('ModeArbiter', () => {
  const DEFAULT = mode(1920, 1080, 30);

  const setup = (): { clock: FakeClock; arbiter: ModeArbiter; changes: VideoMode[] } => {
    const clock = new FakeClock();
    const arbiter = new ModeArbiter({ initial: DEFAULT, downgradeGraceMs: 3000, clock });
    const changes: VideoMode[] = [];
    arbiter.events.on('changed', (changed) => changes.push(changed));
    return { clock, arbiter, changes };
  };

  it('starts at the initial mode and keeps it when consumers ask for exactly that', () => {
    const { arbiter, changes } = setup();
    arbiter.update([DEFAULT]);
    assert.deepEqual(arbiter.mode, DEFAULT);
    assert.deepEqual(changes, []);
  });

  it('combines the largest size with the highest frame rate', () => {
    const { arbiter, changes } = setup();
    arbiter.update([mode(1920, 1080, 30), mode(1280, 720, 60)]);
    assert.deepEqual(changes, [mode(1920, 1080, 60)]);
    assert.deepEqual(arbiter.mode, mode(1920, 1080, 60));
  });

  it('falls back to the highest catalog rate of the size (4K60 → 4K30)', () => {
    const { arbiter, changes } = setup();
    arbiter.update([mode(3840, 2160, 15), mode(1280, 720, 60)]);
    assert.deepEqual(changes, [mode(3840, 2160, 30)]);
  });

  it('applies a larger mode immediately', () => {
    const { arbiter, changes } = setup();
    arbiter.update([DEFAULT]);
    arbiter.update([DEFAULT, mode(2560, 1440, 30)]);
    assert.deepEqual(changes, [mode(2560, 1440, 30)]);
    arbiter.update([DEFAULT, mode(2560, 1440, 30), mode(640, 360, 60)]);
    assert.deepEqual(changes, [mode(2560, 1440, 30), mode(2560, 1440, 60)], 'a higher frame rate is an upgrade');
  });

  it('applies a smaller mode only after the grace period', async () => {
    const { clock, arbiter, changes } = setup();
    arbiter.update([DEFAULT]);
    arbiter.update([mode(1280, 720, 30)]);
    await clock.advance(2999);
    assert.deepEqual(changes, []);
    assert.deepEqual(arbiter.mode, DEFAULT);
    await clock.advance(1);
    assert.deepEqual(changes, [mode(1280, 720, 30)]);
  });

  it('cancels a pending downgrade when the demand rises again', async () => {
    const { clock, arbiter, changes } = setup();
    arbiter.update([DEFAULT]);
    arbiter.update([mode(1280, 720, 30)]);
    await clock.advance(2000);
    arbiter.update([mode(1280, 720, 30), DEFAULT]);
    await clock.advance(10_000);
    assert.deepEqual(changes, []);
    assert.equal(clock.pendingTimers, 0);
  });

  it('measures the grace from when the larger demand ended and applies the latest smaller mode', async () => {
    const { clock, arbiter, changes } = setup();
    arbiter.update([DEFAULT]);
    arbiter.update([mode(1280, 720, 30)]);
    await clock.advance(2000);
    arbiter.update([mode(960, 540, 30)]);
    await clock.advance(999);
    assert.deepEqual(changes, []);
    await clock.advance(1);
    assert.deepEqual(changes, [mode(960, 540, 30)]);
  });

  it('keeps the last mode while nobody is subscribed', async () => {
    const { clock, arbiter, changes } = setup();
    arbiter.update([DEFAULT, mode(1280, 720, 60)]);
    arbiter.update([]);
    await clock.advance(60_000);
    assert.deepEqual(changes, [mode(1920, 1080, 60)]);
    assert.deepEqual(arbiter.mode, mode(1920, 1080, 60));
  });

  it('counts time without consumers towards the grace period', async () => {
    const { clock, arbiter, changes } = setup();
    arbiter.update([DEFAULT]);
    arbiter.update([]);
    await clock.advance(1000);
    arbiter.update([mode(640, 480, 30)]);
    await clock.advance(1999);
    assert.deepEqual(changes, [], 'a quick reopen in another mode keeps the larger mode');
    await clock.advance(1);
    assert.deepEqual(changes, [mode(640, 480, 30)]);

    arbiter.update([]);
    await clock.advance(5000);
    arbiter.update([mode(640, 360, 15)]);
    assert.deepEqual(changes, [mode(640, 480, 30), mode(640, 360, 15)], 'after a long pause the mode applies at once');
  });

  it('restarts the grace when the consumers of a downgraded mode leave', async () => {
    const { clock, arbiter, changes } = setup();
    arbiter.update([DEFAULT]);
    arbiter.update([mode(1280, 720, 30)]);
    await clock.advance(3000);
    assert.deepEqual(changes, [mode(1280, 720, 30)]);
    arbiter.update([]);
    await clock.advance(100);
    arbiter.update([mode(640, 360, 30)]);
    await clock.advance(2899);
    assert.equal(changes.length, 1);
    await clock.advance(1);
    assert.deepEqual(changes.at(-1), mode(640, 360, 30));
  });

  it('gives the first consumer its mode at once', () => {
    const { arbiter, changes } = setup();
    arbiter.update([mode(640, 360, 30)]);
    assert.deepEqual(changes, [mode(640, 360, 30)]);
  });

  it('cancels its timer on dispose', async () => {
    const { clock, arbiter, changes } = setup();
    arbiter.update([DEFAULT]);
    arbiter.update([mode(1280, 720, 30)]);
    arbiter[Symbol.dispose]();
    await clock.advance(10_000);
    assert.deepEqual(changes, []);
    assert.equal(clock.pendingTimers, 0);
  });
});

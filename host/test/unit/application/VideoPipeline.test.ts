import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { VideoPipeline } from '#application/VideoPipeline.ts';
import { MediaError } from '#domain/errors.ts';
import { Emitter } from '#domain/events.ts';
import { selectPlaceholder } from '#domain/video/PlaceholderPolicy.ts';
import type { VideoMode, VideoSize } from '#domain/video/VideoMode.ts';
import { silentLogger, type Logger } from '#ports/Logger.ts';
import type {
  EncodedAccessUnit,
  VideoDecoder,
  VideoDecoderEvents,
  VideoDecoderFactory,
  VideoOutput,
} from '#ports/Video.ts';
import { InMemoryMetrics } from '#infrastructure/metrics/InMemoryMetrics.ts';
import { FakeClock, flushMicrotasks } from '#test/fakes/FakeClock.ts';
import { FakeVideoOutput } from '#test/fakes/FakeMedia.ts';

class ScriptedDecoder implements VideoDecoder {
  readonly events = new Emitter<VideoDecoderEvents>();
  readonly decoded: EncodedAccessUnit[] = [];
  readonly mode: VideoMode;
  /** The hub's ingest size when the decoder was created. */
  readonly ingestMode: VideoSize;
  accept = true;
  disposed = false;

  constructor(mode: VideoMode, ingestMode: VideoSize) {
    this.mode = mode;
    this.ingestMode = ingestMode;
  }

  decode(accessUnit: EncodedAccessUnit): boolean {
    if (!this.accept) return false;
    this.decoded.push(accessUnit);
    return true;
  }

  crash(): void {
    this.events.emit('failed', new MediaError('DECODER_FAILED', 'boom'));
  }

  async [Symbol.asyncDispose](): Promise<void> {
    this.disposed = true;
    await Promise.resolve();
  }
}

class ScriptedFactory implements VideoDecoderFactory {
  readonly created: ScriptedDecoder[] = [];

  async create(output: VideoOutput, mode: VideoMode): Promise<VideoDecoder> {
    await Promise.resolve();
    const decoder = new ScriptedDecoder(mode, output.ingestMode);
    this.created.push(decoder);
    return decoder;
  }
}

const DEFAULT_MODE: VideoMode = { width: 640, height: 360, fpsNum: 30, fpsDen: 1 };
const HD: VideoMode = { width: 1280, height: 720, fpsNum: 30, fpsDen: 1 };
const FULL_HD: VideoMode = { width: 1920, height: 1080, fpsNum: 30, fpsDen: 1 };

const unit = (isIdr: boolean): EncodedAccessUnit => ({
  data: Buffer.from([isIdr ? 5 : 1]),
  isIdr,
  isDisposable: false,
  timestampUs: 0n,
});

async function setup(logger: Logger = silentLogger): Promise<{
  pipeline: VideoPipeline;
  factory: ScriptedFactory;
  clock: FakeClock;
  keyframes: { count: number };
  metrics: InMemoryMetrics;
  output: FakeVideoOutput;
}> {
  const factory = new ScriptedFactory();
  const clock = new FakeClock();
  const keyframes = { count: 0 };
  const metrics = new InMemoryMetrics();
  const output = await FakeVideoOutput.start(DEFAULT_MODE);
  const pipeline = new VideoPipeline({
    output,
    decoders: factory,
    initialMode: DEFAULT_MODE,
    clock,
    logger,
    metrics,
    requestKeyframe: () => {
      keyframes.count++;
    },
    keyframeRequestIntervalMs: 500,
    maxDecoderRestartsPerMinute: 3,
  });
  return { pipeline, factory, clock, keyframes, metrics, output };
}

describe('VideoPipeline', () => {
  it('requests a keyframe when decoding starts and gates until an IDR', async () => {
    const { pipeline, factory, keyframes, output } = await setup();
    await using _output = output;
    pipeline.setActive(true);
    await flushMicrotasks();
    assert.equal(factory.created.length, 1);
    assert.equal(keyframes.count, 1);
    pipeline.onAccessUnit(unit(false), 0);
    pipeline.onAccessUnit(unit(true), 1);
    pipeline.onAccessUnit(unit(false), 2);
    assert.deepEqual(
      factory.created[0]?.decoded.map((au) => au.isIdr),
      [true, false],
    );
    await pipeline[Symbol.asyncDispose]();
  });

  it('drops until the next IDR and rate-limits keyframe requests when the decoder is congested', async () => {
    const { pipeline, factory, clock, keyframes, output } = await setup();
    await using _output = output;
    pipeline.setActive(true);
    await flushMicrotasks();
    const decoder = factory.created[0]!;
    pipeline.onAccessUnit(unit(true), 0);
    decoder.accept = false;
    pipeline.onAccessUnit(unit(false), 1);
    decoder.accept = true;
    pipeline.onAccessUnit(unit(false), 2);
    assert.equal(decoder.decoded.length, 1, 'P-frames after a loss are dropped');
    assert.equal(keyframes.count, 1, 'rate-limited (start request was < 500 ms ago)');
    await clock.advance(500);
    pipeline.onAccessUnit(unit(false), 3);
    assert.equal(keyframes.count, 2);
    pipeline.onAccessUnit(unit(true), 4);
    assert.equal(decoder.decoded.length, 2);
    await pipeline[Symbol.asyncDispose]();
  });

  it('restarts a crashed decoder, but gives up after too many crashes per minute', async () => {
    const { pipeline, factory, output } = await setup();
    await using _output = output;
    pipeline.setActive(true);
    await flushMicrotasks();
    for (let i = 0; i < 3; i++) {
      factory.created.at(-1)?.crash();
      await flushMicrotasks();
    }
    assert.equal(factory.created.length, 4, 'three restarts');
    factory.created.at(-1)?.crash();
    await flushMicrotasks();
    assert.equal(factory.created.length, 4, 'no fourth restart within a minute');
    assert.equal(pipeline.isActive, false);
    assert.ok(factory.created.every((decoder) => decoder.disposed));
    await pipeline[Symbol.asyncDispose]();
  });

  it('restarts the decoder for a new size after the hub switched its ingest size', async () => {
    const { pipeline, factory, keyframes, output } = await setup();
    await using _output = output;
    pipeline.setActive(true);
    await flushMicrotasks();
    pipeline.onAccessUnit(unit(true), 0);
    assert.deepEqual(output.ingestRequests, [], 'the default mode is already the ingest size');
    assert.equal(factory.created[0]?.mode, DEFAULT_MODE);

    pipeline.setMode(HD);
    await flushMicrotasks();
    assert.equal(factory.created[0].disposed, true);
    assert.deepEqual(output.ingestRequests, [{ width: 1280, height: 720 }]);
    assert.equal(factory.created.length, 2);
    const decoder = factory.created[1]!;
    assert.equal(decoder.mode, HD);
    assert.deepEqual(decoder.ingestMode, { width: 1280, height: 720 }, 'decoder starts after the ack');
    assert.equal(keyframes.count, 2, 'a keyframe is requested for the new decoder');
    pipeline.onAccessUnit(unit(false), 1);
    pipeline.onAccessUnit(unit(true), 2);
    assert.deepEqual(
      decoder.decoded.map((au) => au.isIdr),
      [true],
      'the new decoder starts at an IDR',
    );
    await pipeline[Symbol.asyncDispose]();
  });

  it('keeps the decoder when only the frame rate changes', async () => {
    const { pipeline, factory, output } = await setup();
    await using _output = output;
    pipeline.setActive(true);
    await flushMicrotasks();
    pipeline.setMode({ ...DEFAULT_MODE, fpsNum: 60 });
    await flushMicrotasks();
    assert.equal(factory.created.length, 1);
    assert.equal(factory.created[0]?.disposed, false);
    assert.deepEqual(output.ingestRequests, []);
    await pipeline[Symbol.asyncDispose]();
  });

  it('remembers the mode while idle and switches the ingest size before the first decoder', async () => {
    const { pipeline, factory, output } = await setup();
    await using _output = output;
    pipeline.setMode(HD);
    await flushMicrotasks();
    assert.deepEqual(output.ingestRequests, [], 'nothing happens without demand');
    pipeline.setActive(true);
    await flushMicrotasks();
    assert.deepEqual(output.ingestRequests, [{ width: 1280, height: 720 }]);
    assert.equal(factory.created.length, 1);
    const decoder = factory.created[0]!;
    assert.equal(decoder.mode, HD);
    assert.deepEqual(decoder.ingestMode, { width: 1280, height: 720 });
    await pipeline[Symbol.asyncDispose]();
  });

  it('switches once to the latest mode when modes change in a burst', async () => {
    const { pipeline, factory, output } = await setup();
    await using _output = output;
    pipeline.setActive(true);
    await flushMicrotasks();
    pipeline.setMode(HD);
    pipeline.setMode(FULL_HD);
    await flushMicrotasks();
    assert.deepEqual(output.ingestRequests, [{ width: 1920, height: 1080 }]);
    assert.deepEqual(
      factory.created.map((decoder) => decoder.mode),
      [DEFAULT_MODE, FULL_HD],
    );
    await pipeline[Symbol.asyncDispose]();
  });

  it('follows a mode change that arrives while the hub is switching (latest wins)', async () => {
    const { pipeline, factory, output } = await setup();
    await using _output = output;
    pipeline.setActive(true);
    await flushMicrotasks();
    output.holdIngest = true;
    pipeline.setMode(HD);
    await flushMicrotasks();
    assert.deepEqual(output.ingestRequests, [{ width: 1280, height: 720 }]);
    pipeline.setMode(FULL_HD);
    output.acknowledgeIngest();
    await flushMicrotasks();
    assert.deepEqual(output.ingestRequests, [
      { width: 1280, height: 720 },
      { width: 1920, height: 1080 },
    ]);
    assert.equal(factory.created.length, 1, 'no decoder for the superseded mode');
    output.acknowledgeIngest();
    await flushMicrotasks();
    assert.deepEqual(
      factory.created.map((decoder) => decoder.mode),
      [DEFAULT_MODE, FULL_HD],
    );
    await pipeline[Symbol.asyncDispose]();
  });

  it('abandons a pending ingest switch quietly when disposed', async () => {
    const errors: string[] = [];
    const logger: Logger = { ...silentLogger, child: () => logger, error: (message) => errors.push(message) };
    const { pipeline, factory, output } = await setup(logger);
    await using _output = output;
    pipeline.setActive(true);
    await flushMicrotasks();
    output.holdIngest = true;
    pipeline.setMode(HD);
    await flushMicrotasks();
    await pipeline[Symbol.asyncDispose]();
    assert.equal(factory.created.length, 1);
    assert.deepEqual(errors, []);
  });

  it('forwards placeholder decisions to the output', async () => {
    const { pipeline, output } = await setup();
    await using _output = output;
    pipeline.updatePlaceholder({
      deviceAttached: false,
      sessionReady: false,
      videoState: undefined,
      videoReason: undefined,
    });
    pipeline.updatePlaceholder({
      deviceAttached: true,
      sessionReady: true,
      videoState: 'running',
      videoReason: undefined,
    });
    assert.deepEqual(output.placeholders, ['no-device', null]);
    await pipeline[Symbol.asyncDispose]();
  });
});

describe('selectPlaceholder', () => {
  const base = { deviceAttached: true, sessionReady: true, videoState: undefined, videoReason: undefined } as const;
  it('maps session and device video state to placeholders', () => {
    assert.equal(selectPlaceholder({ ...base, deviceAttached: false }), 'no-device');
    assert.equal(selectPlaceholder({ ...base, sessionReady: false }), 'app-closed');
    assert.equal(selectPlaceholder({ ...base, videoState: 'interrupted', videoReason: 'background' }), 'background');
    assert.equal(
      selectPlaceholder({ ...base, videoState: 'interrupted', videoReason: 'inUseByAnotherClient' }),
      'paused',
    );
    assert.equal(selectPlaceholder({ ...base, videoState: 'error' }), 'paused');
    assert.equal(selectPlaceholder({ ...base, videoState: 'running' }), null);
  });
});

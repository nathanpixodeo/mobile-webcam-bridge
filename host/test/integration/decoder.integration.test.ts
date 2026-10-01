import assert from 'node:assert/strict';
import { setTimeout as sleep } from 'node:timers/promises';
import { before, describe, it } from 'node:test';
import { VideoPipeline } from '#application/VideoPipeline.ts';
import { describeAccessUnit } from '#domain/h264/AnnexB.ts';
import { silentLogger } from '#ports/Logger.ts';
import { FfmpegDecoderFactory } from '#infrastructure/ffmpeg/FfmpegH264Decoder.ts';
import { InMemoryMetrics } from '#infrastructure/metrics/InMemoryMetrics.ts';
import { SystemClock } from '#infrastructure/system/SystemClock.ts';
import { FakeVideoOutput } from '#test/fakes/FakeMedia.ts';
import { ffmpegAvailable, h264Fixture, splitAccessUnits } from '#test/support/h264Fixture.ts';
import { waitFor } from '#test/support/waitFor.ts';

const MODE = { width: 320, height: 240, fpsNum: 30, fpsDen: 1 };

describe(
  'VideoPipeline with the real ffmpeg decoder',
  { skip: !ffmpegAvailable() && 'ffmpeg with libx264 required' },
  () => {
    let accessUnits: Buffer[] = [];

    before(() => {
      accessUnits = splitAccessUnits(h264Fixture({ width: 320, height: 240, fps: 30, seconds: 2 }));
    });

    async function feed(pipeline: VideoPipeline, units: readonly Buffer[], startSeq = 0): Promise<void> {
      let seq = startSeq;
      for (const data of units) {
        pipeline.onAccessUnit(
          { data, isIdr: describeAccessUnit(data).isIdr, isDisposable: false, timestampUs: 0n },
          seq++,
        );
        await sleep(5);
      }
    }

    function createPipeline(
      output: FakeVideoOutput,
      keyframeRequests: { count: number },
      metrics = new InMemoryMetrics(),
    ): VideoPipeline {
      return new VideoPipeline({
        output,
        decoders: new FfmpegDecoderFactory({ ffmpegPath: 'ffmpeg', hwaccel: 'none', logger: silentLogger }),
        clock: new SystemClock(),
        logger: silentLogger,
        metrics,
        requestKeyframe: () => {
          keyframeRequests.count++;
        },
        keyframeRequestIntervalMs: 200,
        maxDecoderRestartsPerMinute: 3,
      });
    }

    it('writes exact-size NV12 frames into the ingest pipe', async () => {
      await using output = await FakeVideoOutput.start(MODE);
      const keyframeRequests = { count: 0 };
      await using pipeline = createPipeline(output, keyframeRequests);
      pipeline.setActive(true);
      await waitFor(() => keyframeRequests.count === 1, 'keyframe request on decoder start', 15_000);
      await feed(pipeline, accessUnits);
      await waitFor(() => output.frames >= accessUnits.length - 1, `${accessUnits.length - 1} decoded frames`, 30_000);
      assert.equal(output.writerConnections, 1);
    });

    it('drops access units until the first IDR', async () => {
      await using output = await FakeVideoOutput.start(MODE);
      const keyframeRequests = { count: 0 };
      const metrics = new InMemoryMetrics();
      await using pipeline = createPipeline(output, keyframeRequests, metrics);
      pipeline.setActive(true);
      await waitFor(() => keyframeRequests.count === 1, 'decoder started', 15_000);
      // Start mid-GOP: frames 1..29 are P-frames, frame 30 is the next IDR.
      await feed(pipeline, accessUnits.slice(1));
      const dropped = metrics.drain().counters['video.droppedAwaitingIdr'] ?? 0;
      assert.equal(dropped, 29);
      await waitFor(() => output.frames >= accessUnits.length - 31, 'frames after the IDR', 30_000);
    });

    it('stops the decoder when demand ends', async () => {
      await using output = await FakeVideoOutput.start(MODE);
      const keyframeRequests = { count: 0 };
      await using pipeline = createPipeline(output, keyframeRequests);
      pipeline.setActive(true);
      await waitFor(() => keyframeRequests.count === 1, 'decoder started', 15_000);
      await feed(pipeline, accessUnits.slice(0, 10));
      await waitFor(() => output.frames >= 8, 'some frames', 30_000);
      pipeline.setActive(false);
      await sleep(300);
      const framesAfterStop = output.frames;
      await feed(pipeline, accessUnits.slice(10, 20), 10);
      await sleep(300);
      assert.equal(output.frames, framesAfterStop);
    });
  },
);

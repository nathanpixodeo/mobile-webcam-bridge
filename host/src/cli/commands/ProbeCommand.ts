import { createWriteStream } from 'node:fs';
import { mkdir } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { finished } from 'node:stream/promises';
import { ProbeRecorder, type ProbeSummary } from '#application/ProbeRecorder.ts';
import {
  createDeviceInfrastructure,
  createSessionFactory,
  startAudioParams,
  startVideoParams,
} from '#composition/createBridgeApp.ts';
import { WavWriter } from '#infrastructure/system/WavWriter.ts';
import { ExitCode, type Command, type CommandContext } from '../Command.ts';

/** Records the raw device streams to disk and prints stream statistics. */
export class ProbeCommand implements Command {
  readonly name = 'probe';
  readonly summary = 'Record raw phone streams (video.h264, audio.wav) and print statistics';
  readonly options = {
    record: { type: 'string' },
    seconds: { type: 'string' },
    width: { type: 'string' },
    height: { type: 'string' },
    fps: { type: 'string' },
    'no-audio': { type: 'boolean' },
  } as const;

  async run(context: CommandContext): Promise<number> {
    const { config, options, terminal, logger, clock } = context;
    const directory = resolve(
      typeof options['record'] === 'string' ? options['record'] : join('recordings', timestamp()),
    );
    const seconds = Number(typeof options['seconds'] === 'string' ? options['seconds'] : '10');
    const number = (name: string, fallback: number): number =>
      typeof options[name] === 'string' ? Number(options[name]) : fallback;
    const mode = {
      width: number('width', config.camera.width),
      height: number('height', config.camera.height),
      fpsNum: number('fps', config.camera.fps),
      fpsDen: 1,
    };
    await mkdir(directory, { recursive: true });

    const infrastructure = createDeviceInfrastructure(context);
    await using watcher = infrastructure.watcher;
    const video = createWriteStream(join(directory, 'video.h264'));
    const packets = createWriteStream(join(directory, 'packets.jsonl'));
    await using audio = await WavWriter.create(join(directory, 'audio.wav'), 48_000, 1);
    const audioWrites: Promise<void>[] = [];

    const abort = new AbortController();
    const onSignal = (): void => {
      abort.abort();
    };
    process.once('SIGINT', onSignal);
    try {
      const recorder = new ProbeRecorder({
        watcher,
        createSession: createSessionFactory(context, infrastructure.tunnels),
        clock,
        logger,
      });
      const summary = await recorder.record(
        {
          seconds,
          video: startVideoParams(config, mode),
          audio: options['no-audio'] === true ? null : startAudioParams(config),
          waitForDeviceMs: 30_000,
          signal: abort.signal,
        },
        {
          writeVideo: (accessUnit) => video.write(accessUnit),
          writeAudio: (samples) => audioWrites.push(audio.write(samples)),
          writeRecord: (record) =>
            packets.write(
              `${JSON.stringify(record, (_, value: unknown) => (typeof value === 'bigint' ? value.toString() : value))}\n`,
            ),
        },
      );
      await Promise.all(audioWrites);
      this.#print(terminal, summary, directory);
      return ExitCode.Ok;
    } finally {
      process.off('SIGINT', onSignal);
      video.end();
      packets.end();
      await Promise.all([finished(video), finished(packets)]);
    }
  }

  #print(terminal: CommandContext['terminal'], summary: ProbeSummary, directory: string): void {
    const fixed = (value: number | undefined, digits = 1): string =>
      value === undefined ? 'n/a' : value.toFixed(digits);
    terminal.table(
      ['Metric', 'Value'],
      [
        ['Device', summary.device],
        ['App', summary.app ?? 'n/a'],
        ['Duration', `${fixed(summary.seconds)} s`],
        ['Video', `${summary.accessUnits} AUs, ${fixed(summary.fps)} fps, ${fixed(summary.kbps, 0)} kbps`],
        ['Keyframes', `${summary.keyframes} (mean interval ${fixed(summary.meanKeyframeIntervalMs, 0)} ms)`],
        ['Video seq gaps', String(summary.videoSeqGaps)],
        ['Audio', `${summary.audioSamples} samples, clock ${fixed(summary.audioRatePpm, 0)} ppm vs host`],
        ['Audio seq gaps', String(summary.audioSeqGaps)],
        ['RTT', `${fixed(summary.rttMs, 2)} ms`],
      ],
    );
    terminal.line(`Recorded to ${directory} (play with: ffplay -f h264 "${join(directory, 'video.h264')}")`);
  }
}

function timestamp(): string {
  return new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
}

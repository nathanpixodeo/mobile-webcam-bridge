import { randomBytes } from 'node:crypto';
import { createServer, type Server } from 'node:net';
import { Emitter, type EventSource } from '#domain/events.ts';
import type { PlaceholderKind } from '#domain/video/PlaceholderPolicy.ts';
import { nv12FrameBytes, type VideoMode } from '#domain/video/VideoMode.ts';
import type { AudioSink, AudioSinkEvents, AudioSinkStatus } from '#ports/Audio.ts';
import type { VideoOutput, VideoOutputEvents } from '#ports/Video.ts';

/**
 * Stand-in for the video hub: hosts the ingest pipe in-process (security is irrelevant in
 * tests), counts complete NV12 frames, and lets the test set the consumer count.
 */
export class FakeVideoOutput implements VideoOutput {
  readonly #events = new Emitter<VideoOutputEvents>();
  readonly #server: Server;
  readonly #mode: VideoMode;
  readonly #ingestPath: string;
  readonly placeholders: (PlaceholderKind | null)[] = [];
  #consumers = 0;
  #bytes = 0;
  #writers = 0;

  private constructor(server: Server, mode: VideoMode, ingestPath: string) {
    this.#server = server;
    this.#mode = mode;
    this.#ingestPath = ingestPath;
  }

  static async start(mode: VideoMode): Promise<FakeVideoOutput> {
    const ingestPath = String.raw`\\.\pipe\mobile-webcam-bridge-test-ingest-` + randomBytes(6).toString('hex');
    const server = createServer();
    const output = new FakeVideoOutput(server, mode, ingestPath);
    server.on('connection', (socket) => {
      output.#writers++;
      socket.on('data', (chunk: Buffer) => {
        output.#bytes += chunk.length;
      });
      socket.on('error', () => undefined);
    });
    await new Promise<void>((resolve) => server.listen(ingestPath, resolve));
    return output;
  }

  get ingestPath(): string {
    return this.#ingestPath;
  }

  get mode(): VideoMode {
    return this.#mode;
  }

  get consumers(): number {
    return this.#consumers;
  }

  get events(): EventSource<VideoOutputEvents> {
    return this.#events;
  }

  /** Complete frames received on the ingest pipe. */
  get frames(): number {
    return Math.floor(this.#bytes / nv12FrameBytes(this.#mode));
  }

  get writerConnections(): number {
    return this.#writers;
  }

  setConsumers(count: number): void {
    this.#consumers = count;
    this.#events.emit('consumersChanged', count);
  }

  setPlaceholder(kind: PlaceholderKind | null): void {
    if (this.placeholders.at(-1) !== kind) this.placeholders.push(kind);
  }

  async [Symbol.asyncDispose](): Promise<void> {
    await new Promise<void>((resolve) => {
      this.#server.close(() => {
        resolve();
      });
    });
  }
}

/** Records everything written; the test drives the driver status. */
export class FakeAudioSink implements AudioSink {
  readonly #events = new Emitter<AudioSinkEvents>();
  readonly chunks: Int16Array[] = [];

  get events(): EventSource<AudioSinkEvents> {
    return this.#events;
  }

  readonly sampleRate = 48_000;

  get samplesWritten(): number {
    return this.chunks.reduce((total, chunk) => total + chunk.length, 0);
  }

  write(samples: Int16Array): boolean {
    this.chunks.push(samples.slice());
    return true;
  }

  emitStatus(status: AudioSinkStatus): void {
    this.#events.emit('status', status);
  }

  async [Symbol.asyncDispose](): Promise<void> {
    await Promise.resolve();
  }
}

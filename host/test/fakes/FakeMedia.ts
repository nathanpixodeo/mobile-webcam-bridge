import { randomBytes } from 'node:crypto';
import { createServer, type Server, type Socket } from 'node:net';
import { abortReason } from '#domain/abort.ts';
import { Emitter, type EventSource } from '#domain/events.ts';
import type { PlaceholderKind } from '#domain/video/PlaceholderPolicy.ts';
import { nv12FrameBytes, type VideoMode, type VideoSize } from '#domain/video/VideoMode.ts';
import type { AudioSink, AudioSinkEvents, AudioSinkStatus } from '#ports/Audio.ts';
import { NO_CONSUMERS, type ConsumerDemand, type VideoOutput, type VideoOutputEvents } from '#ports/Video.ts';

/**
 * Stand-in for the video hub: hosts the ingest pipe in-process (security is irrelevant in
 * tests), counts complete NV12 frames of the current ingest size, and lets the test set the
 * consumers and their modes.
 */
export class FakeVideoOutput implements VideoOutput {
  readonly #events = new Emitter<VideoOutputEvents>();
  readonly #server: Server;
  readonly #defaultMode: VideoMode;
  readonly #ingestPath: string;
  readonly #sockets = new Set<Socket>();
  readonly placeholders: (PlaceholderKind | null)[] = [];
  /** Every size passed to `setIngestMode`, in call order. */
  readonly ingestRequests: VideoSize[] = [];
  /** When set, `setIngestMode` waits for `acknowledgeIngest()` instead of acknowledging at once. */
  holdIngest = false;
  #pendingAcknowledge: (() => void) | undefined;
  #ingestMode: VideoSize;
  #consumers: ConsumerDemand = NO_CONSUMERS;
  #bytes = 0;
  #writers = 0;

  private constructor(server: Server, defaultMode: VideoMode, ingestPath: string) {
    this.#server = server;
    this.#defaultMode = defaultMode;
    this.#ingestMode = { width: defaultMode.width, height: defaultMode.height };
    this.#ingestPath = ingestPath;
  }

  static async start(defaultMode: VideoMode): Promise<FakeVideoOutput> {
    const ingestPath = String.raw`\\.\pipe\mobile-webcam-bridge-test-ingest-` + randomBytes(6).toString('hex');
    const server = createServer();
    const output = new FakeVideoOutput(server, defaultMode, ingestPath);
    server.on('connection', (socket) => {
      output.#writers++;
      output.#sockets.add(socket);
      socket.on('data', (chunk: Buffer) => {
        output.#bytes += chunk.length;
      });
      socket.on('close', () => {
        output.#sockets.delete(socket);
      });
      socket.on('error', () => undefined);
    });
    await new Promise<void>((resolve) => server.listen(ingestPath, resolve));
    return output;
  }

  get ingestPath(): string {
    return this.#ingestPath;
  }

  get defaultMode(): VideoMode {
    return this.#defaultMode;
  }

  get ingestMode(): VideoSize {
    return this.#ingestMode;
  }

  get consumers(): ConsumerDemand {
    return this.#consumers;
  }

  get events(): EventSource<VideoOutputEvents> {
    return this.#events;
  }

  /** Complete frames received on the ingest pipe since the last ingest size change. */
  get frames(): number {
    return Math.floor(this.#bytes / nv12FrameBytes(this.#ingestMode));
  }

  get writerConnections(): number {
    return this.#writers;
  }

  /** One mode per consumer. */
  setConsumers(modes: readonly VideoMode[]): void {
    this.#consumers = { count: modes.length, modes };
    this.#events.emit('consumersChanged', this.#consumers);
  }

  setPlaceholder(kind: PlaceholderKind | null): void {
    if (this.placeholders.at(-1) !== kind) this.placeholders.push(kind);
  }

  async setIngestMode(size: VideoSize, signal: AbortSignal): Promise<void> {
    signal.throwIfAborted();
    this.ingestRequests.push({ width: size.width, height: size.height });
    if (this.holdIngest) {
      await new Promise<void>((resolve, reject) => {
        this.#pendingAcknowledge = resolve;
        signal.addEventListener(
          'abort',
          () => {
            reject(abortReason(signal));
          },
          { once: true },
        );
      });
    } else {
      await Promise.resolve();
    }
    // Like the hub: the current writer and any partial frame are dropped.
    for (const socket of this.#sockets) socket.destroy();
    this.#bytes = 0;
    this.#ingestMode = { width: size.width, height: size.height };
  }

  /** Acknowledges the `setIngestMode` call held by `holdIngest`. */
  acknowledgeIngest(): void {
    const acknowledge = this.#pendingAcknowledge;
    this.#pendingAcknowledge = undefined;
    acknowledge?.();
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

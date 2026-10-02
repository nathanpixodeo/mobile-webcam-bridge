import { createServer, type Server, type Socket } from 'node:net';
import { describeAccessUnit } from '#domain/h264/AnnexB.ts';
import { encodeJsonPayload, parseJsonPayload, StartVideoSchema, type StartVideo } from '#domain/protocol/messages.ts';
import { decodePong, encodePacket, encodePong, type Packet } from '#domain/protocol/PacketCodec.ts';
import { PacketReader } from '#domain/protocol/PacketReader.ts';
import { AudioFlag, PacketType, VideoFlag } from '#domain/protocol/PacketType.ts';
import type { JsonValue } from '#domain/protocol/CanonicalJson.ts';
import { sameParams } from '#domain/session/StreamReconciler.ts';

export interface FakeCompanionOptions {
  readonly accessUnits: readonly Buffer[];
  readonly fps: number;
  /** Major protocol version announced in Hello (tests the mismatch path). */
  readonly protocolMajor?: number;
  /** Fixed TCP port; an ephemeral port by default. */
  readonly port?: number;
}

/**
 * Device side of the wire protocol over plain TCP, standing in for the iOS app in tests. Loops
 * the given access units at `fps` while video is requested and emits a 1 kHz tone while audio is
 * requested. A keyframe request restarts the loop at its first access unit (an IDR). A StartVideo
 * with new parameters reconfigures like the real app: a new VideoConfig reporting the requested
 * size, then the loop from its IDR (the access units themselves keep the fixture's size).
 */
export class FakeCompanion implements AsyncDisposable {
  readonly #options: FakeCompanionOptions;
  readonly #server: Server;
  readonly #received: Packet[] = [];
  #socket: Socket | undefined;
  #connections = 0;
  #silent = false;
  #seq = new Map<number, number>();
  #video: StartVideo | undefined;
  #configId = 0;
  #videoTimer: NodeJS.Timeout | undefined;
  #audioTimer: NodeJS.Timeout | undefined;
  #pingTimer: NodeJS.Timeout | undefined;
  #auIndex = 0;
  #audioPhase = 0;
  #audioFirst = true;

  private constructor(options: FakeCompanionOptions, server: Server) {
    this.#options = options;
    this.#server = server;
  }

  static async start(options: FakeCompanionOptions): Promise<FakeCompanion> {
    const server = createServer();
    const companion = new FakeCompanion(options, server);
    server.on('connection', (socket) => {
      companion.#accept(socket);
    });
    await new Promise<void>((resolve) => server.listen(options.port ?? 0, '127.0.0.1', resolve));
    return companion;
  }

  get port(): number {
    const address = this.#server.address();
    if (address === null || typeof address === 'string') throw new Error('not listening');
    return address.port;
  }

  get connections(): number {
    return this.#connections;
  }

  /** Host packets received so far. */
  received(type?: number): Packet[] {
    return type === undefined ? [...this.#received] : this.#received.filter((packet) => packet.type === type);
  }

  get videoRunning(): boolean {
    return this.#videoTimer !== undefined;
  }

  /** Parameters of the running video stream. */
  get video(): StartVideo | undefined {
    return this.#video;
  }

  get audioRunning(): boolean {
    return this.#audioTimer !== undefined;
  }

  /** Stops sending anything and ignores the host (simulates a hung app). */
  goSilent(): void {
    this.#silent = true;
    this.#stopStreams();
    if (this.#pingTimer !== undefined) clearInterval(this.#pingTimer);
  }

  dropConnection(): void {
    this.#socket?.destroy();
  }

  sendStatus(status: JsonValue): void {
    this.#send(PacketType.Status, encodeJsonPayload(status));
  }

  async [Symbol.asyncDispose](): Promise<void> {
    this.#stopStreams();
    if (this.#pingTimer !== undefined) clearInterval(this.#pingTimer);
    this.#socket?.destroy();
    await new Promise<void>((resolve) => {
      this.#server.close(() => {
        resolve();
      });
    });
  }

  #accept(socket: Socket): void {
    this.#socket?.destroy();
    this.#stopStreams();
    this.#socket = socket;
    this.#connections++;
    this.#silent = false;
    this.#seq = new Map();
    const reader = new PacketReader();
    socket.setNoDelay(true);
    socket.on('data', (chunk: Buffer) => {
      if (this.#silent || socket !== this.#socket) return;
      for (const packet of reader.push(chunk)) this.#handle(packet);
    });
    socket.on('error', () => undefined);
    socket.on('close', () => {
      if (socket === this.#socket) this.#stopStreams();
    });
    this.#send(
      PacketType.Hello,
      encodeJsonPayload({
        protocol: { major: this.#options.protocolMajor ?? 1, minor: 0 },
        role: 'device',
        app: { name: 'fake-companion', version: '0.0.0', build: 'test', gitSha: 'test' },
        device: { model: 'FakePhone1,1', name: 'Fake', os: 'iOS 26.0' },
        features: ['audio.pcm', 'video.h264'],
      }),
    );
    if (this.#pingTimer !== undefined) clearInterval(this.#pingTimer);
    this.#pingTimer = setInterval(() => {
      this.#send(PacketType.Ping);
    }, 200);
  }

  #handle(packet: Packet): void {
    this.#received.push({ ...packet, payload: Buffer.from(packet.payload) });
    switch (packet.type) {
      case PacketType.Ping:
        this.#send(PacketType.Pong, encodePong({ echoT0: packet.timestampUs, t1: nowUs() }));
        break;
      case PacketType.Pong:
        decodePong(packet.payload);
        break;
      case PacketType.StartVideo:
        this.#startVideo(parseJsonPayload(StartVideoSchema, packet.payload, 'StartVideo'));
        break;
      case PacketType.StopVideo:
        this.#stopVideo();
        break;
      case PacketType.RequestKeyframe:
        this.#auIndex = 0;
        break;
      case PacketType.StartAudio:
        this.#startAudio();
        break;
      case PacketType.StopAudio:
        this.#stopAudio();
        break;
      default:
        break;
    }
  }

  #startVideo(params: StartVideo): void {
    if (this.#video !== undefined && sameParams(params, this.#video)) return;
    this.#video = params;
    this.#send(
      PacketType.VideoConfig,
      encodeJsonPayload({
        configId: ++this.#configId,
        codec: 'h264',
        profile: 'high',
        width: params.width,
        height: params.height,
        fps: params.fps,
        bitrateKbps: params.bitrateKbps,
        rotationDeg: 0,
        mirrored: params.mirror,
        encoder: params.encoder,
        camera: params.camera,
      }),
    );
    this.#auIndex = 0;
    if (this.#videoTimer !== undefined) return;
    this.#videoTimer = setInterval(() => {
      const accessUnit = this.#options.accessUnits[this.#auIndex];
      if (accessUnit === undefined) return;
      this.#auIndex = (this.#auIndex + 1) % this.#options.accessUnits.length;
      const info = describeAccessUnit(accessUnit);
      const flags = (info.isIdr ? VideoFlag.Idr : 0) | (info.hasParameterSets ? VideoFlag.ParameterSets : 0);
      this.#send(PacketType.VideoAccessUnit, accessUnit, flags);
    }, 1000 / this.#options.fps);
  }

  #startAudio(): void {
    if (this.#audioTimer !== undefined) return;
    this.#send(
      PacketType.AudioConfig,
      encodeJsonPayload({ configId: 1, format: 's16le', sampleRate: 48000, channels: 1 }),
    );
    this.#audioFirst = true;
    this.#audioTimer = setInterval(() => {
      const samples = new Int16Array(960);
      for (let i = 0; i < samples.length; i++) {
        samples[i] = Math.round(8000 * Math.sin(this.#audioPhase));
        this.#audioPhase += (2 * Math.PI * 1000) / 48000;
      }
      this.#send(PacketType.AudioChunk, Buffer.from(samples.buffer), this.#audioFirst ? AudioFlag.Discontinuity : 0);
      this.#audioFirst = false;
    }, 20);
  }

  #stopVideo(): void {
    if (this.#videoTimer !== undefined) clearInterval(this.#videoTimer);
    this.#videoTimer = undefined;
    this.#video = undefined;
  }

  #stopAudio(): void {
    if (this.#audioTimer !== undefined) clearInterval(this.#audioTimer);
    this.#audioTimer = undefined;
  }

  #stopStreams(): void {
    this.#stopVideo();
    this.#stopAudio();
  }

  #send(type: number, payload?: Buffer, flags = 0): void {
    const socket = this.#socket;
    if (socket === undefined || socket.destroyed || this.#silent) return;
    const seq = this.#seq.get(type) ?? 0;
    this.#seq.set(type, (seq + 1) >>> 0);
    socket.write(
      encodePacket({ type, flags, seq, timestampUs: nowUs(), ...(payload === undefined ? {} : { payload }) }),
    );
  }
}

function nowUs(): bigint {
  return process.hrtime.bigint() / 1000n;
}

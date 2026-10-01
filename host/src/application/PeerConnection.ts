import type { Clock } from '#domain/clock.ts';
import {
  ProtocolError,
  RemoteError,
  TransportError,
  isBridgeError,
  toError,
  type BridgeError,
} from '#domain/errors.ts';
import { Emitter, type EventSource } from '#domain/events.ts';
import { describeAccessUnit } from '#domain/h264/AnnexB.ts';
import { decodePong, encodePacket, encodePong, type Packet } from '#domain/protocol/PacketCodec.ts';
import { PacketReader } from '#domain/protocol/PacketReader.ts';
import {
  AudioFlag,
  PacketType,
  PROTOCOL_MAJOR,
  PROTOCOL_MINOR,
  VideoFlag,
  packetTypeName,
} from '#domain/protocol/PacketType.ts';
import {
  AudioConfigSchema,
  ErrorMessageSchema,
  HelloSchema,
  LogSchema,
  StatusSchema,
  VideoConfigSchema,
  encodeJsonPayload,
  parseJsonPayload,
  type AudioConfig,
  type DeviceLog,
  type DeviceStatus,
  type Hello,
  type StartAudio,
  type StartVideo,
  type VideoConfig,
} from '#domain/protocol/messages.ts';
import { ClockOffsetEstimator } from '#domain/session/ClockOffsetEstimator.ts';
import type { DeviceTunnel } from '#ports/DeviceTunnelFactory.ts';
import type { Logger } from '#ports/Logger.ts';
import type { EncodedAccessUnit } from '#ports/Video.ts';

export interface AudioChunk {
  /** Mono s16 samples at 48 kHz. */
  readonly samples: Int16Array;
  readonly discontinuity: boolean;
  readonly timestampUs: bigint;
  readonly seq: number;
}

export interface PeerConnectionEvents {
  videoConfig: [config: VideoConfig];
  accessUnit: [accessUnit: EncodedAccessUnit, seq: number];
  audioConfig: [config: AudioConfig];
  audioChunk: [chunk: AudioChunk];
  status: [status: DeviceStatus];
  log: [entry: DeviceLog, timestampUs: bigint];
}

export interface PeerConnectionOptions {
  readonly tunnel: DeviceTunnel;
  readonly clock: Clock;
  readonly logger: Logger;
  readonly hello: Omit<Hello, 'protocol' | 'role' | 'device'>;
  readonly pingIntervalMs: number;
  readonly heartbeatTimeoutMs: number;
}

/**
 * One live protocol connection to the companion app: framing, handshake, heartbeat, clock
 * offset, and typed message dispatch. Lives exactly as long as its tunnel; `closed` resolves
 * with the reason the connection ended.
 */
export class PeerConnection implements AsyncDisposable {
  readonly #options: PeerConnectionOptions;
  readonly #reader = new PacketReader();
  readonly #events = new Emitter<PeerConnectionEvents>();
  readonly #seq = new Map<number, number>();
  readonly #clockOffset = new ClockOffsetEstimator();
  readonly #closed: Promise<BridgeError>;
  #resolveClosed!: (reason: BridgeError) => void;
  #remoteHello: Hello | undefined;
  #helloWaiter: { resolve: (hello: Hello) => void; reject: (error: BridgeError) => void } | undefined;
  #lastReceiveMs: number;
  #heartbeat: Disposable | undefined;
  #isClosed = false;

  constructor(options: PeerConnectionOptions) {
    this.#options = options;
    this.#lastReceiveMs = options.clock.nowMs();
    this.#closed = new Promise<BridgeError>((resolve) => {
      this.#resolveClosed = resolve;
    });
    const stream = options.tunnel.stream;
    stream.on('data', (chunk: Buffer) => {
      this.#receive(chunk);
    });
    stream.on('error', (error) => {
      this.#close(new TransportError('CONNECTION_CLOSED', `Tunnel error: ${error.message}`, { cause: error }));
    });
    stream.on('close', () => {
      this.#close(new TransportError('CONNECTION_CLOSED', 'The companion app closed the connection'));
    });
    // Tunnels are delivered paused (see DeviceTunnel); start reading now that listeners exist.
    stream.resume();
  }

  get events(): EventSource<PeerConnectionEvents> {
    return this.#events;
  }

  /** Resolves with the reason once the connection has ended. */
  get closed(): Promise<BridgeError> {
    return this.#closed;
  }

  get remoteHello(): Hello | undefined {
    return this.#remoteHello;
  }

  get clockOffset(): ClockOffsetEstimator {
    return this.#clockOffset;
  }

  /** Sends our Hello and waits for the device's. Starts the heartbeat on success. */
  async handshake(timeoutMs: number): Promise<Hello> {
    const helloPayload: Hello = {
      ...this.#options.hello,
      protocol: { major: PROTOCOL_MAJOR, minor: PROTOCOL_MINOR },
      role: 'host',
    };
    this.#send(PacketType.Hello, encodeJsonPayload(helloPayload));
    const hello = await new Promise<Hello>((resolve, reject) => {
      if (this.#remoteHello !== undefined) {
        resolve(this.#remoteHello);
        return;
      }
      const timer = this.#options.clock.setTimeout(() => {
        this.#helloWaiter = undefined;
        reject(new TransportError('HANDSHAKE_TIMEOUT', `No Hello from the companion app within ${timeoutMs} ms`));
      }, timeoutMs);
      this.#helloWaiter = {
        resolve: (value) => {
          timer[Symbol.dispose]();
          resolve(value);
        },
        reject: (error) => {
          timer[Symbol.dispose]();
          reject(error);
        },
      };
    });
    if (hello.protocol.major !== PROTOCOL_MAJOR) {
      const error = ProtocolError.versionMismatch(PROTOCOL_MAJOR, hello.protocol.major);
      this.#send(
        PacketType.Error,
        encodeJsonPayload({ code: 'VERSION_MISMATCH', message: error.message, fatal: true }),
      );
      this.#close(error);
      throw error;
    }
    this.#startHeartbeat();
    return hello;
  }

  startVideo(params: StartVideo): void {
    this.#send(PacketType.StartVideo, encodeJsonPayload(params));
  }

  stopVideo(): void {
    this.#send(PacketType.StopVideo);
  }

  requestKeyframe(): void {
    this.#send(PacketType.RequestKeyframe);
  }

  startAudio(params: StartAudio): void {
    this.#send(PacketType.StartAudio, encodeJsonPayload(params));
  }

  stopAudio(): void {
    this.#send(PacketType.StopAudio);
  }

  /** Flushes pending writes (e.g. final Stop commands) for up to 500 ms, then closes the tunnel. */
  async [Symbol.asyncDispose](): Promise<void> {
    if (!this.#isClosed) {
      const stream = this.#options.tunnel.stream;
      this.#close(new TransportError('CONNECTION_CLOSED', 'Connection closed by the host'), false);
      await new Promise<void>((resolve) => {
        const timer = this.#options.clock.setTimeout(resolve, 500);
        stream.end(() => {
          timer[Symbol.dispose]();
          resolve();
        });
      });
    }
    await this.#options.tunnel[Symbol.asyncDispose]();
  }

  #send(type: number, payload?: Buffer, flags = 0): void {
    if (this.#isClosed) return;
    const seq = this.#seq.get(type) ?? 0;
    this.#seq.set(type, (seq + 1) >>> 0);
    const packet = encodePacket({
      type,
      flags,
      seq,
      timestampUs: this.#options.clock.nowUs(),
      ...(payload ? { payload } : {}),
    });
    this.#options.tunnel.stream.write(packet);
  }

  #receive(chunk: Buffer): void {
    if (this.#isClosed) return;
    this.#lastReceiveMs = this.#options.clock.nowMs();
    try {
      for (const packet of this.#reader.push(chunk)) this.#dispatch(packet);
    } catch (error) {
      this.#close(isBridgeError(error) ? error : ProtocolError.violation('BAD_MESSAGE', toError(error).message, error));
    }
  }

  #dispatch(packet: Packet): void {
    if (packet.type !== PacketType.Hello && this.#remoteHello === undefined) {
      // SPEC §2: nothing is meaningful before the peer's Hello.
      this.#options.logger.debug('Ignoring packet before Hello', { type: packetTypeName(packet.type) });
      return;
    }
    switch (packet.type) {
      case PacketType.Hello:
        this.#onHello(parseJsonPayload(HelloSchema, packet.payload, 'Hello'));
        break;
      case PacketType.Ping:
        this.#send(PacketType.Pong, encodePong({ echoT0: packet.timestampUs, t1: this.#options.clock.nowUs() }));
        break;
      case PacketType.Pong: {
        const pong = decodePong(packet.payload);
        this.#clockOffset.add({
          t0: pong.echoT0,
          t1: pong.t1,
          t2: packet.timestampUs,
          t3: this.#options.clock.nowUs(),
        });
        break;
      }
      case PacketType.Error: {
        const message = parseJsonPayload(ErrorMessageSchema, packet.payload, 'Error');
        const error = new RemoteError(message.code, message.message, message.fatal);
        this.#options.logger.warn('Companion app reported an error', { code: message.code, message: message.message });
        if (message.fatal) this.#close(error);
        break;
      }
      case PacketType.VideoConfig:
        this.#events.emit('videoConfig', parseJsonPayload(VideoConfigSchema, packet.payload, 'VideoConfig'));
        break;
      case PacketType.VideoAccessUnit:
        this.#events.emit('accessUnit', this.#toAccessUnit(packet), packet.seq);
        break;
      case PacketType.AudioConfig:
        this.#events.emit('audioConfig', parseJsonPayload(AudioConfigSchema, packet.payload, 'AudioConfig'));
        break;
      case PacketType.AudioChunk:
        this.#events.emit('audioChunk', {
          samples: pcmFromPayload(packet.payload),
          discontinuity: (packet.flags & AudioFlag.Discontinuity) !== 0,
          timestampUs: packet.timestampUs,
          seq: packet.seq,
        });
        break;
      case PacketType.Status:
        this.#events.emit('status', parseJsonPayload(StatusSchema, packet.payload, 'Status'));
        break;
      case PacketType.Log:
        this.#events.emit('log', parseJsonPayload(LogSchema, packet.payload, 'Log'), packet.timestampUs);
        break;
      default:
        // Host-to-device types and unknown types are ignored (forward compatibility).
        break;
    }
  }

  #toAccessUnit(packet: Packet): EncodedAccessUnit {
    // Copy: access units outlive the socket chunk they arrived in (decoder backlog).
    const data = Buffer.from(packet.payload);
    const flaggedIdr = (packet.flags & VideoFlag.Idr) !== 0;
    return {
      data,
      isIdr: flaggedIdr || describeAccessUnit(data).isIdr,
      isDisposable: (packet.flags & VideoFlag.Disposable) !== 0,
      timestampUs: packet.timestampUs,
    };
  }

  #onHello(hello: Hello): void {
    if (this.#remoteHello !== undefined) return;
    this.#remoteHello = hello;
    this.#options.logger.info('Companion app connected', {
      app: `${hello.app.name} ${hello.app.version} (${hello.app.build})`,
      device: hello.device === undefined ? undefined : `${hello.device.model} ${hello.device.os}`,
      protocol: `${hello.protocol.major}.${hello.protocol.minor}`,
    });
    this.#helloWaiter?.resolve(hello);
    this.#helloWaiter = undefined;
  }

  #startHeartbeat(): void {
    const { clock, pingIntervalMs, heartbeatTimeoutMs } = this.#options;
    this.#heartbeat = clock.setInterval(() => {
      if (clock.nowMs() - this.#lastReceiveMs > heartbeatTimeoutMs) {
        this.#close(
          new TransportError('HEARTBEAT_TIMEOUT', `No data from the companion app for ${heartbeatTimeoutMs} ms`),
        );
        return;
      }
      this.#send(PacketType.Ping);
    }, pingIntervalMs);
  }

  #close(reason: BridgeError, destroyStream = true): void {
    if (this.#isClosed) return;
    this.#isClosed = true;
    this.#heartbeat?.[Symbol.dispose]();
    this.#helloWaiter?.reject(reason);
    this.#helloWaiter = undefined;
    if (destroyStream) this.#options.tunnel.stream.destroy();
    this.#resolveClosed(reason);
    this.#events.clear();
  }
}

/** Views (or copies, when misaligned) an s16le payload as Int16Array. x64/ARM64 are little-endian. */
function pcmFromPayload(payload: Buffer): Int16Array {
  const sampleCount = payload.length >>> 1;
  if (payload.byteOffset % 2 === 0) return new Int16Array(payload.buffer, payload.byteOffset, sampleCount).slice();
  const samples = new Int16Array(sampleCount);
  Buffer.from(samples.buffer).set(payload.subarray(0, sampleCount * 2));
  return samples;
}

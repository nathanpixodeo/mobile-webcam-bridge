import type { Clock } from '#domain/clock.ts';
import { abortReason, isAborted, whenAborted } from '#domain/abort.ts';
import { TransportError, isAbortError, isBridgeError, toError, type BridgeError } from '#domain/errors.ts';
import { Emitter, type EventSource } from '#domain/events.ts';
import type { AudioConfig, DeviceLog, DeviceStatus, Hello, VideoConfig } from '#domain/protocol/messages.ts';
import type { ReconnectPolicy } from '#domain/session/ReconnectPolicy.ts';
import {
  NO_STREAMS,
  StreamReconciler,
  type DesiredStreams,
  type StreamCommand,
} from '#domain/session/StreamReconciler.ts';
import type { DeviceTunnel, DeviceTunnelFactory } from '#ports/DeviceTunnelFactory.ts';
import type { MobileDevice } from '#ports/DeviceWatcher.ts';
import type { Logger } from '#ports/Logger.ts';
import type { EncodedAccessUnit } from '#ports/Video.ts';
import { PeerConnection, type AudioChunk } from './PeerConnection.ts';

export type SessionState =
  | { readonly kind: 'idle' }
  | { readonly kind: 'connecting' }
  | { readonly kind: 'handshaking' }
  | { readonly kind: 'ready'; readonly peer: Hello }
  | { readonly kind: 'backoff'; readonly delayMs: number; readonly reason: BridgeError }
  | { readonly kind: 'stalled'; readonly reason: BridgeError }
  | { readonly kind: 'disposed' };

export interface DeviceSessionEvents {
  state: [state: SessionState];
  videoConfig: [config: VideoConfig];
  accessUnit: [accessUnit: EncodedAccessUnit, seq: number];
  audioConfig: [config: AudioConfig];
  audioChunk: [chunk: AudioChunk];
  status: [status: DeviceStatus];
  remoteLog: [entry: DeviceLog, hostTimeUs: bigint | undefined];
}

export interface SessionTimings {
  readonly connectTimeoutMs: number;
  readonly handshakeTimeoutMs: number;
  readonly pingIntervalMs: number;
  readonly heartbeatTimeoutMs: number;
}

export interface DeviceSessionOptions {
  readonly device: MobileDevice;
  readonly port: number;
  readonly tunnels: DeviceTunnelFactory;
  readonly policy: ReconnectPolicy;
  readonly timings: SessionTimings;
  readonly hello: ConstructorParameters<typeof PeerConnection>[0]['hello'];
  readonly clock: Clock;
  readonly logger: Logger;
}

/**
 * Supervises the connection to one phone for as long as it stays attached: connect, handshake,
 * stream, and on any failure decide (via `ReconnectPolicy`) whether to retry, back off, or wait.
 * Desired streams are level-triggered: after every (re)connect they are requested again.
 */
export class DeviceSession implements AsyncDisposable {
  readonly #options: DeviceSessionOptions;
  readonly #events = new Emitter<DeviceSessionEvents>();
  readonly #abort = new AbortController();
  readonly #reconciler = new StreamReconciler();
  readonly #logger: Logger;
  #state: SessionState = { kind: 'idle' };
  #desired: DesiredStreams = NO_STREAMS;
  #peer: PeerConnection | undefined;
  #loop: Promise<void> | undefined;

  constructor(options: DeviceSessionOptions) {
    this.#options = options;
    this.#logger = options.logger.child({ device: options.device.id, platform: options.device.platform });
  }

  get device(): MobileDevice {
    return this.#options.device;
  }

  get state(): SessionState {
    return this.#state;
  }

  get events(): EventSource<DeviceSessionEvents> {
    return this.#events;
  }

  /** Round-trip time and clock offset of the live connection, if any. */
  get timing(): { readonly rttUs: bigint | undefined; readonly offsetUs: bigint | undefined } {
    return { rttUs: this.#peer?.clockOffset.rttUs, offsetUs: this.#peer?.clockOffset.offsetUs };
  }

  start(): void {
    this.#loop ??= this.#supervise(this.#abort.signal);
  }

  setDesiredStreams(desired: DesiredStreams): void {
    this.#desired = desired;
    if (this.#state.kind === 'ready') this.#apply(this.#reconciler.reconcile(desired));
  }

  requestKeyframe(): void {
    if (this.#state.kind === 'ready') this.#peer?.requestKeyframe();
  }

  async [Symbol.asyncDispose](): Promise<void> {
    if (this.#state.kind === 'ready') this.#apply(this.#reconciler.reconcile(NO_STREAMS));
    this.#abort.abort();
    await this.#loop;
    this.#setState({ kind: 'disposed' });
    this.#events.clear();
  }

  async #supervise(signal: AbortSignal): Promise<void> {
    const { clock, policy } = this.#options;
    while (!signal.aborted) {
      let peer: PeerConnection | undefined;
      let connectedAtMs: number | undefined;
      try {
        this.#setState({ kind: 'connecting' });
        peer = this.#createPeer(await this.#openTunnel(signal));
        this.#setState({ kind: 'handshaking' });
        const hello = await peer.handshake(this.#options.timings.handshakeTimeoutMs);
        connectedAtMs = clock.nowMs();
        policy.onConnected();
        this.#peer = peer;
        this.#reconciler.reset();
        this.#setState({ kind: 'ready', peer: hello });
        this.#apply(this.#reconciler.reconcile(this.#desired));
        throw await Promise.race([peer.closed, whenAborted(signal)]);
      } catch (error) {
        if (isAborted(signal) || isAbortError(error)) break;
        const failure = classifyFailure(error, connectedAtMs === undefined);
        if (connectedAtMs !== undefined) policy.onDisconnected(clock.nowMs() - connectedAtMs);
        if (!(await this.#recover(failure, signal))) break;
      } finally {
        this.#peer = undefined;
        await peer?.[Symbol.asyncDispose]();
      }
    }
  }

  /** Applies the reconnect policy. Returns false when the loop should end. */
  async #recover(failure: BridgeError, signal: AbortSignal): Promise<boolean> {
    const decision = this.#options.policy.decide(failure);
    switch (decision.kind) {
      case 'retry': {
        const log = decision.quiet ? this.#logger.debug.bind(this.#logger) : this.#logger.info.bind(this.#logger);
        log(
          failure.code === 'APP_NOT_REACHABLE'
            ? 'Waiting for the Mobile Webcam app to be opened on the phone'
            : 'Connection lost, retrying',
          {
            reason: failure.message,
            delayMs: decision.delayMs,
          },
        );
        this.#setState({ kind: 'backoff', delayMs: decision.delayMs, reason: failure });
        try {
          await this.#options.clock.sleep(decision.delayMs, signal);
          return true;
        } catch {
          return false;
        }
      }
      case 'waitForReattach':
      case 'giveUp':
        this.#logger.warn('Session stopped', { reason: failure.message, code: failure.code });
        this.#setState({ kind: 'stalled', reason: failure });
        await whenAborted(signal).catch(() => undefined);
        return false;
    }
  }

  async #openTunnel(signal: AbortSignal): Promise<DeviceTunnel> {
    const { tunnels, device, port, clock, timings } = this.#options;
    const attempt = new AbortController();
    const onAbort = (): void => {
      attempt.abort(abortReason(signal));
    };
    signal.addEventListener('abort', onAbort, { once: true });
    const timer = clock.setTimeout(() => {
      attempt.abort(
        new TransportError('CONNECTION_CLOSED', `Opening the tunnel timed out after ${timings.connectTimeoutMs} ms`),
      );
    }, timings.connectTimeoutMs);
    try {
      return await tunnels.open(device, port, attempt.signal);
    } catch (error) {
      if (attempt.signal.aborted && !signal.aborted) throw abortReason(attempt.signal);
      throw error;
    } finally {
      timer[Symbol.dispose]();
      signal.removeEventListener('abort', onAbort);
    }
  }

  #createPeer(tunnel: DeviceTunnel): PeerConnection {
    const peer = new PeerConnection({
      tunnel,
      clock: this.#options.clock,
      logger: this.#logger.child({ tunnel: tunnel.label }),
      hello: this.#options.hello,
      pingIntervalMs: this.#options.timings.pingIntervalMs,
      heartbeatTimeoutMs: this.#options.timings.heartbeatTimeoutMs,
    });
    peer.events.on('videoConfig', (config) => {
      this.#events.emit('videoConfig', config);
    });
    peer.events.on('accessUnit', (accessUnit, seq) => {
      this.#events.emit('accessUnit', accessUnit, seq);
    });
    peer.events.on('audioConfig', (config) => {
      this.#events.emit('audioConfig', config);
    });
    peer.events.on('audioChunk', (chunk) => {
      this.#events.emit('audioChunk', chunk);
    });
    peer.events.on('status', (status) => {
      this.#events.emit('status', status);
    });
    peer.events.on('log', (entry, deviceTimeUs) => {
      this.#events.emit('remoteLog', entry, peer.clockOffset.toLocal(deviceTimeUs));
    });
    return peer;
  }

  #apply(commands: readonly StreamCommand[]): void {
    const peer = this.#peer;
    if (peer === undefined) return;
    for (const command of commands) {
      this.#logger.debug('Stream command', { command: command.kind });
      switch (command.kind) {
        case 'startVideo':
          peer.startVideo(command.params);
          break;
        case 'stopVideo':
          peer.stopVideo();
          break;
        case 'startAudio':
          peer.startAudio(command.params);
          break;
        case 'stopAudio':
          peer.stopAudio();
          break;
      }
    }
  }

  #setState(state: SessionState): void {
    if (this.#state.kind === state.kind && state.kind !== 'backoff') return;
    this.#state = state;
    this.#events.emit('state', state);
  }
}

/**
 * Normalises a failure. A tunnel that closes before the handshake completed means the
 * companion app is not (yet) serving the protocol: ADB accepts `tcp:` streams and closes them
 * when nothing listens, unlike usbmux which refuses them, so both are reported the same way.
 */
function classifyFailure(error: unknown, beforeHandshake: boolean): BridgeError {
  const failure = isBridgeError(error)
    ? error
    : new TransportError('CONNECTION_CLOSED', toError(error).message, { cause: error });
  if (beforeHandshake && failure.code === 'CONNECTION_CLOSED') {
    return new TransportError('APP_NOT_REACHABLE', `The companion app is not answering (${failure.message})`, {
      cause: failure,
    });
  }
  return failure;
}

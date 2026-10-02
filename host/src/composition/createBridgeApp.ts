import { readFileSync } from 'node:fs';
import { AudioDriftCompensator } from '#domain/audio/AudioDriftCompensator.ts';
import type { Clock } from '#domain/clock.ts';
import { MediaError, toError } from '#domain/errors.ts';
import type { StartAudio, StartVideo } from '#domain/protocol/messages.ts';
import { ReconnectPolicy } from '#domain/session/ReconnectPolicy.ts';
import { bitrateKbpsFor } from '#domain/video/BitratePolicy.ts';
import { PLACEHOLDER_KINDS } from '#domain/video/PlaceholderPolicy.ts';
import { describeMode, framesPerSecond, type VideoMode } from '#domain/video/VideoMode.ts';
import type { AudioSink } from '#ports/Audio.ts';
import type { DeviceTunnelFactory } from '#ports/DeviceTunnelFactory.ts';
import type { DeviceWatcher, MobileDevice } from '#ports/DeviceWatcher.ts';
import type { Logger } from '#ports/Logger.ts';
import type { NativeHelper } from '#ports/NativeHelper.ts';
import type { VideoDecoderFactory, VideoOutput } from '#ports/Video.ts';
import { AudioPipeline } from '#application/AudioPipeline.ts';
import { BridgeService } from '#application/BridgeService.ts';
import { DeviceManager } from '#application/DeviceManager.ts';
import { DeviceSession } from '#application/DeviceSession.ts';
import { VideoPipeline } from '#application/VideoPipeline.ts';
import { AdbClient } from '#infrastructure/adb/AdbClient.ts';
import { AdbDeviceWatcher } from '#infrastructure/adb/AdbDeviceWatcher.ts';
import { AdbServerLauncher, locateAdb } from '#infrastructure/adb/AdbServerLauncher.ts';
import { AdbTunnelFactory } from '#infrastructure/adb/AdbTunnelFactory.ts';
import type { BridgeConfig } from '#infrastructure/config/ConfigSchema.ts';
import { FfmpegDecoderFactory } from '#infrastructure/ffmpeg/FfmpegH264Decoder.ts';
import {
  FfmpegLocator,
  FfmpegPlaceholderRenderer,
  PLACEHOLDER_FRAME_SIZE,
} from '#infrastructure/ffmpeg/FfmpegToolchain.ts';
import { InMemoryMetrics } from '#infrastructure/metrics/InMemoryMetrics.ts';
import { BridgeNativeCli } from '#infrastructure/native/BridgeNativeCli.ts';
import { locateBridgeNative } from '#infrastructure/native/BridgeNativeLocator.ts';
import { BridgeNativeMicSink } from '#infrastructure/native/BridgeNativeMicSink.ts';
import { VideoHubOutput } from '#infrastructure/native/VideoHubOutput.ts';
import { AppPaths } from '#infrastructure/system/AppPaths.ts';
import { StaticDeviceWatcher } from '#infrastructure/tcp/StaticDeviceWatcher.ts';
import { TcpTunnelFactory } from '#infrastructure/tcp/TcpTunnelFactory.ts';
import { CompositeDeviceWatcher } from '#infrastructure/tunnel/CompositeDeviceWatcher.ts';
import { CompositeTunnelFactory } from '#infrastructure/tunnel/CompositeTunnelFactory.ts';
import { UsbmuxClient } from '#infrastructure/usbmux/UsbmuxClient.ts';
import { UsbmuxDeviceWatcher } from '#infrastructure/usbmux/UsbmuxDeviceWatcher.ts';
import { UsbmuxTunnelFactory } from '#infrastructure/usbmux/UsbmuxTunnelFactory.ts';

export interface AppContext {
  readonly config: BridgeConfig;
  readonly logger: Logger;
  readonly clock: Clock;
}

export const HOST_VERSION: string = (
  JSON.parse(readFileSync(new URL('../../package.json', import.meta.url), 'utf8')) as { version: string }
).version;

export interface DeviceInfrastructure {
  readonly watcher: DeviceWatcher;
  readonly tunnels: DeviceTunnelFactory;
  /** Present when iPhone support is enabled. */
  readonly usbmux: UsbmuxClient | undefined;
  /** Present when Android support is enabled. */
  readonly adb: AdbClient | undefined;
}

/**
 * Device discovery and tunnels for every enabled transport: usbmuxd for iPhones, the ADB server
 * for Android phones (or a single TCP endpoint in development mode).
 */
export function createDeviceInfrastructure(context: AppContext): DeviceInfrastructure {
  const { config, clock, logger } = context;
  if (config.usbmux.tcpHost !== undefined) {
    logger.warn('Development mode: connecting over TCP instead of USB', { host: config.usbmux.tcpHost });
    return {
      watcher: new StaticDeviceWatcher(),
      tunnels: new TcpTunnelFactory(config.usbmux.tcpHost),
      usbmux: undefined,
      adb: undefined,
    };
  }
  const watchers: DeviceWatcher[] = [];
  const factories: DeviceTunnelFactory[] = [];
  let usbmux: UsbmuxClient | undefined;
  let adb: AdbClient | undefined;
  if (config.usbmux.enabled) {
    usbmux = new UsbmuxClient(parseAddress(config.usbmux.address, 27_015));
    watchers.push(new UsbmuxDeviceWatcher({ client: usbmux, clock, logger: logger.child({ component: 'usbmux' }) }));
    factories.push(new UsbmuxTunnelFactory(usbmux));
  }
  if (config.adb.enabled) {
    adb = new AdbClient(parseAddress(config.adb.address, 5037));
    const adbLogger = logger.child({ component: 'adb' });
    const adbPath = config.adb.startServer ? locateAdb(config.adb.path) : undefined;
    watchers.push(
      new AdbDeviceWatcher({
        client: adb,
        clock,
        logger: adbLogger,
        includeNetworkDevices: config.adb.includeNetworkDevices,
        ...(adbPath === undefined ? {} : { launcher: new AdbServerLauncher({ adbPath, logger: adbLogger }) }),
      }),
    );
    factories.push(new AdbTunnelFactory(adb));
  }
  return { watcher: new CompositeDeviceWatcher(watchers), tunnels: new CompositeTunnelFactory(factories), usbmux, adb };
}

function parseAddress(address: string, defaultPort: number): { host: string; port: number } {
  const separator = address.lastIndexOf(':');
  const port = Number(address.slice(separator + 1));
  return {
    host: separator > 0 ? address.slice(0, separator) : '127.0.0.1',
    port: Number.isInteger(port) && port > 0 ? port : defaultPort,
  };
}

export function createSessionFactory(
  context: AppContext,
  tunnels: DeviceTunnelFactory,
): (device: MobileDevice) => DeviceSession {
  const { config, clock, logger } = context;
  const device = config.device;
  return (target) =>
    new DeviceSession({
      device: target,
      port: device.port,
      tunnels,
      clock,
      logger: logger.child({ component: 'session' }),
      policy: new ReconnectPolicy({
        backoff: device.backoff,
        appPollMs: device.appPollMs,
        stableAfterMs: device.stableAfterMs,
      }),
      timings: {
        connectTimeoutMs: device.connectTimeoutMs,
        handshakeTimeoutMs: device.handshakeTimeoutMs,
        pingIntervalMs: device.pingIntervalMs,
        heartbeatTimeoutMs: device.heartbeatTimeoutMs,
      },
      hello: {
        app: {
          name: 'mobile-webcam-bridge-host',
          version: HOST_VERSION,
          build: 'dev',
          gitSha: process.env['MWB_GIT_SHA'] ?? 'unknown',
        },
        features: ['audio.pcm', 'log', 'status', 'video.h264'],
      },
    });
}

export function startVideoParams(config: BridgeConfig, mode: VideoMode): StartVideo {
  return {
    width: mode.width,
    height: mode.height,
    fps: Math.round(framesPerSecond(mode)),
    bitrateKbps: bitrateKbpsFor(mode, {
      bitsPerPixel: config.video.bitsPerPixel,
      overrideKbps: config.video.bitrateKbps,
    }),
    camera: config.video.camera,
    mirror: config.video.mirror,
    orientation: config.video.orientation,
    encoder: config.video.encoder,
  };
}

export function startAudioParams(config: BridgeConfig): StartAudio {
  return { processing: config.audio.processing };
}

export function createNativeHelper(context: AppContext): NativeHelper | undefined {
  const path = locateBridgeNative(context.config.native.bridgeNativePath);
  return path === undefined
    ? undefined
    : new BridgeNativeCli(path, context.logger.child({ component: 'bridge-native' }));
}

/** Test seams: replace any external component. */
export interface BridgeAppOverrides {
  readonly native?: NativeHelper;
  readonly videoOutput?: VideoOutput;
  readonly decoders?: VideoDecoderFactory;
  readonly audioSink?: AudioSink | null;
  readonly devices?: DeviceInfrastructure;
}

/**
 * Composition root of the running bridge. Starts the native helpers, wires every component and
 * returns the service. If anything fails midway, everything already started is torn down.
 */
export async function createBridgeApp(context: AppContext, overrides: BridgeAppOverrides = {}): Promise<BridgeService> {
  const { config, logger, clock } = context;
  await using resources = new AsyncDisposableStack();

  const videoOutput = overrides.videoOutput ?? (await startVideoHub(context, overrides.native, resources));
  if (overrides.videoOutput !== undefined) resources.use(overrides.videoOutput);
  logger.info('Virtual camera ready', { defaultMode: describeMode(videoOutput.defaultMode) });

  const audioSink =
    overrides.audioSink === undefined ? await startMicSink(context, overrides.native) : overrides.audioSink;
  if (audioSink !== null) resources.use(audioSink);

  const devices = overrides.devices ?? createDeviceInfrastructure(context);
  resources.use(devices.watcher);

  const metrics = new InMemoryMetrics();
  const manager = new DeviceManager({
    watcher: devices.watcher,
    createSession: createSessionFactory(context, devices.tunnels),
    logger: logger.child({ component: 'devices' }),
    pinnedId: config.device.id,
  });
  const videoPipeline = new VideoPipeline({
    output: videoOutput,
    initialMode: videoOutput.defaultMode,
    decoders:
      overrides.decoders ??
      new FfmpegDecoderFactory({
        ffmpegPath: config.ffmpeg.path,
        hwaccel: config.video.hwaccel,
        logger: logger.child({ component: 'decoder' }),
      }),
    clock,
    logger: logger.child({ component: 'video' }),
    metrics,
    requestKeyframe: () => {
      manager.activeSession?.requestKeyframe();
    },
    keyframeRequestIntervalMs: config.video.keyframeRequestIntervalMs,
    maxDecoderRestartsPerMinute: config.video.maxDecoderRestartsPerMinute,
  });
  const audio =
    audioSink === null
      ? undefined
      : {
          sink: audioSink,
          pipeline: new AudioPipeline({
            sink: audioSink,
            clock,
            metrics,
            compensator: new AudioDriftCompensator({
              sampleRate: audioSink.sampleRate,
              bytesPerFrame: 2,
              targetFillMs: config.audio.targetBufferMs,
              ...config.audio.drift,
            }),
          }),
        };

  return new BridgeService({
    manager,
    videoOutput,
    videoPipeline,
    audio,
    streams: { video: (mode) => startVideoParams(config, mode), audio: startAudioParams(config) },
    videoStopGraceMs: config.video.stopGraceMs,
    modeDowngradeGraceMs: config.video.modeDowngradeGraceMs,
    audioStopGraceMs: config.audio.stopGraceMs,
    metricsIntervalMs: config.logging.metricsIntervalMs,
    clock,
    logger,
    metrics,
    resources: resources.move(),
  });
}

async function startVideoHub(
  context: AppContext,
  nativeOverride: NativeHelper | undefined,
  resources: AsyncDisposableStack,
): Promise<VideoOutput> {
  const { config, logger } = context;
  const native = nativeOverride ?? createNativeHelper(context);
  if (native === undefined) {
    throw new MediaError(
      'NATIVE_NOT_INSTALLED',
      'bridge-native.exe not found. Build the native components and run `install` first.',
    );
  }
  const status = await native.status();
  if (!status.camera.installed) {
    throw new MediaError(
      'NATIVE_NOT_INSTALLED',
      'The virtual camera is not installed. Run `mobile-webcam-bridge install` first.',
    );
  }
  const ffmpeg = new FfmpegLocator(config.ffmpeg.path, logger);
  logger.debug('Using ffmpeg', { version: await ffmpeg.version() });
  const placeholders = await new FfmpegPlaceholderRenderer({
    ffmpegPath: config.ffmpeg.path,
    assetsDir: AppPaths.placeholderAssetsDir(),
    cacheDir: AppPaths.placeholderCacheDir(),
    logger,
  }).render(PLACEHOLDER_KINDS);
  return resources.use(
    await VideoHubOutput.start({
      executablePath: native.executablePath,
      logger,
      placeholders,
      placeholderSize: PLACEHOLDER_FRAME_SIZE,
    }),
  );
}

async function startMicSink(context: AppContext, nativeOverride: NativeHelper | undefined): Promise<AudioSink | null> {
  const { config, logger } = context;
  if (!config.audio.enabled) return null;
  const native = nativeOverride ?? createNativeHelper(context);
  if (native === undefined) return null;
  try {
    const status = await native.status();
    if (!status.mic.installed || !status.mic.devicePresent) {
      logger.warn(
        'Virtual microphone not installed; running camera-only. Run `mobile-webcam-bridge install` to add it.',
      );
      return null;
    }
    return await BridgeNativeMicSink.start({ executablePath: native.executablePath, logger });
  } catch (error) {
    logger.warn('Virtual microphone unavailable; running camera-only', { error: toError(error).message });
    return null;
  }
}

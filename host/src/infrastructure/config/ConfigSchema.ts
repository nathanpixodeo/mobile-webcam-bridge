import * as z from 'zod';
import {
  AudioProcessingSchema,
  CameraIdSchema,
  EncoderModeSchema,
  OrientationSchema,
} from '#domain/protocol/messages.ts';

const int = z.int();

const BackoffSchema = z
  .object({
    initialMs: int.positive().default(250),
    maxMs: int.positive().default(5000),
    factor: z.number().min(1).default(2),
    jitter: z.number().min(0).max(1).default(0.2),
  })
  .strict();

export const ConfigSchema = z
  .object({
    device: z
      .object({
        /** Pin one phone by iPhone UDID or Android serial; otherwise the first USB phone is used. */
        id: z.string().min(1).optional(),
        port: int.min(1).max(65_535).default(27_100),
        connectTimeoutMs: int.positive().default(3000),
        handshakeTimeoutMs: int.positive().default(3000),
        pingIntervalMs: int.positive().default(1000),
        heartbeatTimeoutMs: int.positive().default(4000),
        appPollMs: int.positive().default(1000),
        stableAfterMs: int.positive().default(10_000),
        backoff: BackoffSchema.prefault({}),
      })
      .strict()
      .prefault({}),
    usbmux: z
      .object({
        /** iPhone support (Apple Mobile Device Service). */
        enabled: z.boolean().default(true),
        /** `host:port` of usbmuxd; change it to reach a host's usbmuxd from a VM. */
        address: z
          .string()
          .regex(/^[\w.-]+:\d+$/)
          .default('127.0.0.1:27015'),
        /** Development: talk to a device-side process over plain TCP instead of usbmux/ADB. */
        tcpHost: z.string().optional(),
      })
      .strict()
      .prefault({}),
    adb: z
      .object({
        /** Android support (ADB server from Android platform-tools). */
        enabled: z.boolean().default(true),
        /** `host:port` of the ADB server. */
        address: z
          .string()
          .regex(/^[\w.-]+:\d+$/)
          .default('127.0.0.1:5037'),
        /** adb.exe used to start the server; found on PATH or in the Android SDK when omitted. */
        path: z.string().min(1).optional(),
        /** Run `adb start-server` when the server is not running. */
        startServer: z.boolean().default(true),
        /** Also use phones connected by wireless debugging. */
        includeNetworkDevices: z.boolean().default(false),
      })
      .strict()
      .prefault({}),
    video: z
      .object({
        bitrateKbps: int.min(500).max(20_000).default(6000),
        camera: CameraIdSchema.default('back.wide'),
        mirror: z.boolean().default(false),
        orientation: OrientationSchema.default('auto'),
        encoder: EncoderModeSchema.default('lowLatency'),
        hwaccel: z.enum(['none', 'd3d11va', 'dxva2']).default('none'),
        stopGraceMs: int.nonnegative().default(3000),
        keyframeRequestIntervalMs: int.positive().default(500),
        maxDecoderRestartsPerMinute: int.positive().default(3),
      })
      .strict()
      .prefault({}),
    audio: z
      .object({
        enabled: z.boolean().default(true),
        processing: AudioProcessingSchema.default('standard'),
        stopGraceMs: int.nonnegative().default(2000),
        targetBufferMs: int.min(10).max(200).default(40),
        drift: z
          .object({
            kp: z.number().nonnegative().default(10),
            ki: z.number().nonnegative().default(2),
            maxPpm: z.number().positive().max(5000).default(1000),
            slewPpmPerSecond: z.number().positive().default(200),
            smoothing: z.number().gt(0).max(1).default(0.2),
            resyncThresholdMs: z.number().positive().default(150),
          })
          .strict()
          .prefault({}),
      })
      .strict()
      .prefault({}),
    camera: z
      .object({
        /** Mode used by `install` (the installed mode in HKLM is the runtime source of truth). */
        width: int.default(1280),
        height: int.default(720),
        fps: int.default(30),
        friendlyName: z.string().min(1).max(64).default('Mobile Webcam'),
        backend: z.enum(['auto', 'mf', 'dshow', 'none']).default('auto'),
      })
      .strict()
      .prefault({}),
    ffmpeg: z
      .object({ path: z.string().min(1).default('ffmpeg') })
      .strict()
      .prefault({}),
    native: z
      .object({ bridgeNativePath: z.string().min(1).optional() })
      .strict()
      .prefault({}),
    logging: z
      .object({
        level: z.enum(['trace', 'debug', 'info', 'warn', 'error', 'silent']).default('info'),
        file: z.boolean().default(true),
        retentionDays: int.positive().default(7),
        metricsIntervalMs: int.positive().default(10_000),
      })
      .strict()
      .prefault({}),
  })
  .strict();

export type BridgeConfig = z.infer<typeof ConfigSchema>;
export type BridgeConfigInput = z.input<typeof ConfigSchema>;

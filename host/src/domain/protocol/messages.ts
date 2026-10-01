/**
 * JSON payload schemas of wire protocol v1 (protocol/SPEC.md §4). Parsing strips unknown keys,
 * which implements the "ignore unknown fields" forward-compatibility rule.
 */
import * as z from 'zod';
import { ProtocolError } from '#domain/errors.ts';
import { encodeCanonicalJson, type JsonValue } from './CanonicalJson.ts';

const integer = z.int();

export const HelloSchema = z.object({
  protocol: z.object({ major: integer.nonnegative(), minor: integer.nonnegative() }),
  role: z.enum(['host', 'device']),
  app: z.object({ name: z.string(), version: z.string(), build: z.string(), gitSha: z.string() }),
  device: z.object({ model: z.string(), name: z.string(), os: z.string() }).optional(),
  features: z.array(z.string()),
});
export type Hello = z.infer<typeof HelloSchema>;

export const ErrorMessageSchema = z.object({
  code: z.string(),
  message: z.string(),
  fatal: z.boolean(),
});
export type ErrorMessage = z.infer<typeof ErrorMessageSchema>;

export const CameraIdSchema = z.enum(['back.wide', 'back.ultraWide', 'back.telephoto', 'front']);
export type CameraId = z.infer<typeof CameraIdSchema>;

export const OrientationSchema = z.enum(['auto', 'landscape', 'portrait']);
export const EncoderModeSchema = z.enum(['lowLatency', 'standard']);
export const AudioProcessingSchema = z.enum(['standard', 'raw', 'voice']);
export type AudioProcessing = z.infer<typeof AudioProcessingSchema>;

export const StartVideoSchema = z.object({
  width: integer.min(160).max(3840),
  height: integer.min(120).max(2160),
  fps: integer.min(15).max(60),
  bitrateKbps: integer.min(500).max(20_000),
  camera: CameraIdSchema,
  mirror: z.boolean(),
  orientation: OrientationSchema,
  encoder: EncoderModeSchema,
});
export type StartVideo = z.infer<typeof StartVideoSchema>;

export const StartAudioSchema = z.object({ processing: AudioProcessingSchema });
export type StartAudio = z.infer<typeof StartAudioSchema>;

export const VideoConfigSchema = z.object({
  configId: integer,
  codec: z.literal('h264'),
  profile: z.string(),
  width: integer.positive(),
  height: integer.positive(),
  fps: integer.positive(),
  bitrateKbps: integer.positive(),
  rotationDeg: integer,
  mirrored: z.boolean(),
  encoder: z.string(),
  camera: z.string(),
});
export type VideoConfig = z.infer<typeof VideoConfigSchema>;

export const AudioConfigSchema = z.object({
  configId: integer,
  format: z.literal('s16le'),
  sampleRate: z.literal(48_000),
  channels: z.literal(1),
});
export type AudioConfig = z.infer<typeof AudioConfigSchema>;

const StreamStateSchema = z.enum(['off', 'starting', 'running', 'interrupted', 'error']);
export type StreamState = z.infer<typeof StreamStateSchema>;
const PermissionSchema = z.enum(['authorized', 'denied', 'restricted', 'notDetermined']);

export const StatusSchema = z.object({
  video: z.object({
    state: StreamStateSchema,
    reason: z.string().optional(),
    width: integer.optional(),
    height: integer.optional(),
    fps: integer.optional(),
    bitrateKbps: integer.optional(),
  }),
  audio: z.object({ state: StreamStateSchema, reason: z.string().optional() }),
  thermal: z.enum(['nominal', 'fair', 'serious', 'critical']),
  battery: z.object({ levelPercent: integer.min(0).max(100), charging: z.boolean() }).optional(),
  lowPower: z.boolean(),
  permissions: z.object({ camera: PermissionSchema, microphone: PermissionSchema }),
  appState: z.enum(['active', 'inactive', 'background']),
});
export type DeviceStatus = z.infer<typeof StatusSchema>;

export const LogSchema = z.object({
  level: z.enum(['debug', 'info', 'warn', 'error']),
  category: z.string(),
  message: z.string(),
});
export type DeviceLog = z.infer<typeof LogSchema>;

/** Parses a JSON payload against `schema`; any failure is a `BAD_MESSAGE` protocol violation. */
export function parseJsonPayload<S extends z.ZodType>(schema: S, payload: Buffer, what: string): z.infer<S> {
  let raw: unknown;
  try {
    raw = JSON.parse(payload.toString('utf8'));
  } catch (error) {
    throw ProtocolError.violation('BAD_MESSAGE', `${what}: payload is not valid JSON`, error);
  }
  const result = schema.safeParse(raw);
  if (!result.success) {
    throw ProtocolError.violation('BAD_MESSAGE', `${what}: ${z.prettifyError(result.error)}`, result.error);
  }
  return result.data;
}

export function encodeJsonPayload(value: JsonValue): Buffer {
  return encodeCanonicalJson(value);
}

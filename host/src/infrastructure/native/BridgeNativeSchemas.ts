/** zod schemas for bridge-native.exe output (protocol/BRIDGE_NATIVE.md). */
import * as z from 'zod';
import { isCatalogMode } from '#domain/video/ModeCatalog.ts';

const int = z.int();

const ModeSchema = z.object({
  width: int.positive(),
  height: int.positive(),
  fpsNum: int.positive(),
  fpsDen: int.positive(),
});

const CapSchema = z.object({ maxWidth: int.positive(), maxHeight: int.positive(), maxFps: int.positive() });

export const NativeFailureSchema = z.object({
  ok: z.literal(false),
  error: z.object({ code: z.string(), message: z.string() }),
});

export const VersionSchema = z.object({ ok: z.literal(true), version: z.string(), abi: int });

export const StatusSchema = z.object({
  ok: z.literal(true),
  installDir: z.string().nullable().optional(),
  os: z.object({ build: int, isWin11: z.boolean() }),
  camera: z.union([
    z.object({ installed: z.literal(false) }),
    z.object({
      installed: z.literal(true),
      backend: z.enum(['mf', 'dshow', 'none']),
      friendlyName: z.string(),
      ...ModeSchema.shape,
      ...CapSchema.shape,
      modes: z.array(ModeSchema),
      pipeName: z.string(),
    }),
  ]),
  mic: z.union([
    z.object({ installed: z.literal(false) }),
    z.object({ installed: z.literal(true), devicePresent: z.boolean(), problemCode: int }),
  ]),
});
export type NativeStatusOutput = z.infer<typeof StatusSchema>;

const StepSchema = z.object({
  name: z.string(),
  ok: z.boolean(),
  error: z.object({ code: z.string(), message: z.string() }).optional(),
});

export const InstallOutputSchema = z.object({
  ok: z.boolean(),
  version: z.string().optional(),
  installDir: z.string().optional(),
  steps: z.array(StepSchema),
  error: z.object({ code: z.string(), message: z.string() }).optional(),
});

export const DoctorOutputSchema = z.object({
  ok: z.boolean(),
  checks: z.array(z.object({ id: z.string(), status: z.enum(['pass', 'warn', 'fail']), message: z.string() })),
});

export const HubEventSchema = z.discriminatedUnion('event', [
  z.object({
    event: z.literal('ready'),
    ingestPipe: z.string(),
    publicPipe: z.string(),
    ...ModeSchema.shape,
    ...CapSchema.shape,
  }),
  z
    .object({
      event: z.literal('consumers'),
      count: int.nonnegative(),
      // The mode arbiter relies on consumers only ever subscribing to catalog modes.
      modes: z.array(ModeSchema.refine(isCatalogMode, 'not a catalog mode')),
    })
    .refine((event) => event.modes.length === event.count, { message: 'one mode per consumer', path: ['modes'] }),
  z.object({ event: z.literal('ingestMode'), width: int.positive(), height: int.positive() }),
  z.object({ event: z.literal('ingest'), connected: z.boolean() }),
  z.object({
    event: z.literal('stats'),
    framesIn: int,
    framesOut: int,
    consumerDrops: int,
    placeholderFrames: int,
    consumers: int,
  }),
  z.object({ event: z.literal('error'), code: z.string(), message: z.string(), fatal: z.boolean() }),
]);
export type HubEvent = z.infer<typeof HubEventSchema>;

export const MicEventSchema = z.discriminatedUnion('event', [
  z.object({ event: z.literal('ready'), abi: int, sampleRate: int, channels: int, capacityBytes: int }),
  z.object({
    event: z.literal('status'),
    bufferedBytes: int.nonnegative(),
    capacityBytes: int.positive(),
    streamActive: z.boolean(),
    underruns: int.nonnegative(),
    overruns: int.nonnegative(),
  }),
  z.object({ event: z.literal('error'), code: z.string(), message: z.string(), fatal: z.boolean() }),
]);
export type MicEvent = z.infer<typeof MicEventSchema>;

export const NativeExitCode = {
  Ok: 0,
  Failure: 1,
  Usage: 2,
  NotInstalled: 3,
  ElevationCancelled: 4,
  NotSupportedOs: 5,
} as const;

/** Wire protocol v1 constants. Specification: protocol/SPEC.md. */

export const PROTOCOL_MAJOR = 1;
export const PROTOCOL_MINOR = 0;

export const PACKET_MAGIC = 0x4d574252; // "MWBR"
export const PACKET_HEADER_SIZE = 24;

export const PacketType = {
  Hello: 0x01,
  Error: 0x02,
  Ping: 0x03,
  Pong: 0x04,
  StartVideo: 0x10,
  StopVideo: 0x11,
  RequestKeyframe: 0x12,
  StartAudio: 0x13,
  StopAudio: 0x14,
  VideoConfig: 0x20,
  VideoAccessUnit: 0x21,
  AudioConfig: 0x30,
  AudioChunk: 0x31,
  Status: 0x40,
  Log: 0x41,
} as const;

export type PacketType = (typeof PacketType)[keyof typeof PacketType];

export const VideoFlag = {
  Idr: 0x01,
  ParameterSets: 0x02,
  Disposable: 0x04,
} as const;

export const AudioFlag = {
  Discontinuity: 0x01,
} as const;

export const JSON_PAYLOAD_LIMIT = 65_536;
export const LOG_PAYLOAD_LIMIT = 8_192;
export const AUDIO_PAYLOAD_LIMIT = 65_536;
export const VIDEO_PAYLOAD_LIMIT = 4_194_304;
export const UNKNOWN_PAYLOAD_LIMIT = 4_194_304;
export const PONG_PAYLOAD_LENGTH = 16;

export type PayloadRule =
  { readonly kind: 'exact'; readonly length: number } | { readonly kind: 'max'; readonly length: number };

const exact = (length: number): PayloadRule => ({ kind: 'exact', length });
const max = (length: number): PayloadRule => ({ kind: 'max', length });

const PAYLOAD_RULES: ReadonlyMap<number, PayloadRule> = new Map<number, PayloadRule>([
  [PacketType.Hello, max(JSON_PAYLOAD_LIMIT)],
  [PacketType.Error, max(JSON_PAYLOAD_LIMIT)],
  [PacketType.Ping, exact(0)],
  [PacketType.Pong, exact(PONG_PAYLOAD_LENGTH)],
  [PacketType.StartVideo, max(JSON_PAYLOAD_LIMIT)],
  [PacketType.StopVideo, exact(0)],
  [PacketType.RequestKeyframe, exact(0)],
  [PacketType.StartAudio, max(JSON_PAYLOAD_LIMIT)],
  [PacketType.StopAudio, exact(0)],
  [PacketType.VideoConfig, max(JSON_PAYLOAD_LIMIT)],
  [PacketType.VideoAccessUnit, max(VIDEO_PAYLOAD_LIMIT)],
  [PacketType.AudioConfig, max(JSON_PAYLOAD_LIMIT)],
  [PacketType.AudioChunk, max(AUDIO_PAYLOAD_LIMIT)],
  [PacketType.Status, max(JSON_PAYLOAD_LIMIT)],
  [PacketType.Log, max(LOG_PAYLOAD_LIMIT)],
]);

/** Payload length rule for `type`; unknown types may carry up to 4 MiB (they are skipped). */
export function payloadRuleFor(type: number): PayloadRule {
  return PAYLOAD_RULES.get(type) ?? max(UNKNOWN_PAYLOAD_LIMIT);
}

export function isKnownPacketType(type: number): type is PacketType {
  return PAYLOAD_RULES.has(type);
}

export function packetTypeName(type: number): string {
  for (const [name, value] of Object.entries(PacketType)) {
    if (value === type) return name;
  }
  return `Unknown(0x${type.toString(16).padStart(2, '0')})`;
}

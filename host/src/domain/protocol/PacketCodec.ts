import { ProtocolError } from '#domain/errors.ts';
import { PACKET_HEADER_SIZE, PACKET_MAGIC, PONG_PAYLOAD_LENGTH, packetTypeName, payloadRuleFor } from './PacketType.ts';

export interface PacketHeader {
  readonly type: number;
  readonly flags: number;
  readonly seq: number;
  readonly payloadLength: number;
  readonly timestampUs: bigint;
}

/** A decoded packet. `payload` may be a view into a larger buffer; copy it if you keep it long. */
export interface Packet {
  readonly type: number;
  readonly flags: number;
  readonly seq: number;
  readonly timestampUs: bigint;
  readonly payload: Buffer;
}

const EMPTY = Buffer.alloc(0);

export function encodePacket(packet: {
  readonly type: number;
  readonly flags?: number;
  readonly seq: number;
  readonly timestampUs: bigint;
  readonly payload?: Uint8Array;
}): Buffer {
  const payload = packet.payload ?? EMPTY;
  const out = Buffer.allocUnsafe(PACKET_HEADER_SIZE + payload.length);
  out.writeUInt32BE(PACKET_MAGIC, 0);
  out.writeUInt8(packet.type, 4);
  out.writeUInt8(packet.flags ?? 0, 5);
  out.writeUInt16BE(0, 6);
  out.writeUInt32BE(packet.seq >>> 0, 8);
  out.writeUInt32BE(payload.length, 12);
  out.writeBigInt64BE(packet.timestampUs, 16);
  out.set(payload, PACKET_HEADER_SIZE);
  return out;
}

/** Decodes and validates a header (magic + per-type length rule). Throws `ProtocolError`. */
export function decodeHeader(bytes: Uint8Array): PacketHeader {
  if (bytes.length < PACKET_HEADER_SIZE) {
    throw ProtocolError.violation('BAD_MESSAGE', `Header needs ${PACKET_HEADER_SIZE} bytes, got ${bytes.length}`);
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, PACKET_HEADER_SIZE);
  const magic = view.getUint32(0);
  if (magic !== PACKET_MAGIC) {
    throw ProtocolError.violation('BAD_MAGIC', `Bad packet magic 0x${magic.toString(16).padStart(8, '0')}`);
  }
  const header: PacketHeader = {
    type: view.getUint8(4),
    flags: view.getUint8(5),
    seq: view.getUint32(8),
    payloadLength: view.getUint32(12),
    timestampUs: view.getBigInt64(16),
  };
  const rule = payloadRuleFor(header.type);
  if (rule.kind === 'exact' && header.payloadLength !== rule.length) {
    throw ProtocolError.violation(
      'BAD_PAYLOAD_LENGTH',
      `${packetTypeName(header.type)} requires a ${rule.length}-byte payload, got ${header.payloadLength}`,
    );
  }
  if (rule.kind === 'max' && header.payloadLength > rule.length) {
    throw ProtocolError.violation(
      'PAYLOAD_TOO_LARGE',
      `${packetTypeName(header.type)} payload of ${header.payloadLength} bytes exceeds ${rule.length}`,
    );
  }
  return header;
}

export interface PongPayload {
  /** `timestampUs` of the Ping being answered (sender clock). */
  readonly echoT0: bigint;
  /** Responder clock when the Ping arrived. */
  readonly t1: bigint;
}

export function encodePong(pong: PongPayload): Buffer {
  const out = Buffer.allocUnsafe(PONG_PAYLOAD_LENGTH);
  out.writeBigInt64BE(pong.echoT0, 0);
  out.writeBigInt64BE(pong.t1, 8);
  return out;
}

export function decodePong(payload: Buffer): PongPayload {
  if (payload.length !== PONG_PAYLOAD_LENGTH) {
    throw ProtocolError.violation('BAD_PAYLOAD_LENGTH', `Pong payload must be ${PONG_PAYLOAD_LENGTH} bytes`);
  }
  return { echoT0: payload.readBigInt64BE(0), t1: payload.readBigInt64BE(8) };
}

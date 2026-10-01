import { ByteQueue } from './ByteQueue.ts';
import { decodeHeader, type Packet, type PacketHeader } from './PacketCodec.ts';
import { PACKET_HEADER_SIZE } from './PacketType.ts';

/**
 * Incremental packet parser: feed arbitrary chunks, get complete packets. Throws a
 * `ProtocolError` on the first framing violation; after that the reader must be discarded
 * (the protocol has no resynchronisation).
 */
export class PacketReader {
  readonly #queue = new ByteQueue();
  #pendingHeader: PacketHeader | undefined;
  #failed = false;

  /** Bytes buffered but not yet returned as packets. */
  get bufferedBytes(): number {
    return this.#queue.length;
  }

  push(chunk: Buffer): Packet[] {
    if (this.#failed) throw new Error('PacketReader is unusable after a protocol violation');
    this.#queue.append(chunk);
    const packets: Packet[] = [];
    try {
      for (;;) {
        if (this.#pendingHeader === undefined) {
          if (this.#queue.length < PACKET_HEADER_SIZE) break;
          this.#pendingHeader = decodeHeader(this.#queue.take(PACKET_HEADER_SIZE));
        }
        const header = this.#pendingHeader;
        if (this.#queue.length < header.payloadLength) break;
        const payload = this.#queue.take(header.payloadLength);
        this.#pendingHeader = undefined;
        packets.push({
          type: header.type,
          flags: header.flags,
          seq: header.seq,
          timestampUs: header.timestampUs,
          payload,
        });
      }
    } catch (error) {
      this.#failed = true;
      this.#queue.clear();
      throw error;
    }
    return packets;
  }
}

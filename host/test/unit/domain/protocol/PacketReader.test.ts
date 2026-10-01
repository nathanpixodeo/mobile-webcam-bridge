import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { ProtocolError } from '#domain/errors.ts';
import { decodePong, encodePacket, encodePong, type Packet } from '#domain/protocol/PacketCodec.ts';
import { PacketReader } from '#domain/protocol/PacketReader.ts';
import { hex, packetVectors, seededRandom } from '#test/support/vectors.ts';

const vectors = packetVectors();

function readAll(chunks: readonly Buffer[]): Packet[] {
  const reader = new PacketReader();
  return chunks.flatMap((chunk) => reader.push(chunk));
}

function simplify(packet: Packet): { type: number; flags: number; seq: number; payloadHex: string } {
  return { type: packet.type, flags: packet.flags, seq: packet.seq, payloadHex: packet.payload.toString('hex') };
}

describe('PacketReader / PacketCodec against golden vectors', () => {
  for (const vector of vectors.valid) {
    it(`decodes and re-encodes "${vector.name}"`, () => {
      const [packet, ...rest] = readAll([hex(vector.hex)]);
      assert.equal(rest.length, 0);
      assert.ok(packet);
      assert.equal(packet.type, vector.header.type);
      assert.equal(packet.flags, vector.header.flags);
      assert.equal(packet.seq, vector.header.seq);
      assert.equal(packet.payload.length, vector.header.payloadLength);
      assert.equal(packet.timestampUs, BigInt(vector.header.timestampUs));

      switch (vector.payload.kind) {
        case 'json':
          assert.equal(packet.payload.toString('utf8'), vector.payload.text);
          break;
        case 'pong':
          assert.deepEqual(decodePong(packet.payload), {
            echoT0: BigInt(vector.payload.echoT0),
            t1: BigInt(vector.payload.t1),
          });
          break;
        case 'hex':
          assert.equal(packet.payload.toString('hex'), vector.payload.hex);
          break;
        case 'none':
          assert.equal(packet.payload.length, 0);
          break;
      }

      const encoded = encodePacket({
        type: packet.type,
        flags: packet.flags,
        seq: packet.seq,
        timestampUs: packet.timestampUs,
        payload: packet.payload,
      });
      assert.equal(encoded.toString('hex'), vector.hex);
    });
  }

  for (const vector of vectors.invalid) {
    it(`rejects "${vector.name}" with ${vector.error}`, () => {
      assert.throws(
        () => readAll([hex(vector.hex)]),
        (error: unknown) => error instanceof ProtocolError && error.violation === vector.error,
      );
    });
  }

  for (const stream of vectors.streams) {
    const bytes = hex(stream.hex);

    it(`parses stream "${stream.name}" in one chunk`, () => {
      assert.deepEqual(readAll([bytes]).map(simplify), stream.expect);
    });

    it(`parses stream "${stream.name}" split at every byte boundary`, () => {
      for (let cut = 1; cut < bytes.length; cut++) {
        assert.deepEqual(
          readAll([bytes.subarray(0, cut), bytes.subarray(cut)]).map(simplify),
          stream.expect,
          `cut=${cut}`,
        );
      }
    });

    it(`parses stream "${stream.name}" one byte at a time`, () => {
      const chunks = [...bytes].map((byte) => Buffer.from([byte]));
      assert.deepEqual(readAll(chunks).map(simplify), stream.expect);
    });

    it(`parses stream "${stream.name}" with seeded random splits`, () => {
      const random = seededRandom(0xc0ffee);
      for (let round = 0; round < 200; round++) {
        const chunks: Buffer[] = [];
        let offset = 0;
        while (offset < bytes.length) {
          const size = 1 + Math.floor(random() * 40);
          chunks.push(bytes.subarray(offset, offset + size));
          offset += size;
        }
        assert.deepEqual(readAll(chunks).map(simplify), stream.expect, `round=${round}`);
      }
    });
  }
});

describe('PacketReader behaviour', () => {
  it('keeps partial data buffered until the packet completes', () => {
    const packet = encodePacket({ type: 0x21, seq: 1, timestampUs: 5n, payload: Buffer.alloc(1000, 7) });
    const reader = new PacketReader();
    assert.deepEqual(reader.push(packet.subarray(0, 500)), []);
    assert.equal(reader.bufferedBytes, 500 - 24);
    const [decoded] = reader.push(packet.subarray(500));
    assert.equal(decoded?.payload.length, 1000);
    assert.equal(reader.bufferedBytes, 0);
  });

  it('becomes unusable after a violation', () => {
    const reader = new PacketReader();
    assert.throws(() => reader.push(Buffer.alloc(24, 0xff)), ProtocolError);
    assert.throws(() => reader.push(Buffer.alloc(1)), /unusable/);
  });

  it('round-trips Pong payloads including negative timestamps', () => {
    const pong = { echoT0: -42n, t1: 9_007_199_254_740_993n };
    assert.deepEqual(decodePong(encodePong(pong)), pong);
  });
});

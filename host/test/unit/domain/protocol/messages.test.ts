import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import type * as z from 'zod';
import { ProtocolError } from '#domain/errors.ts';
import { canonicalJson, type JsonValue } from '#domain/protocol/CanonicalJson.ts';
import {
  AudioConfigSchema,
  ErrorMessageSchema,
  HelloSchema,
  LogSchema,
  parseJsonPayload,
  StartAudioSchema,
  StartVideoSchema,
  StatusSchema,
  VideoConfigSchema,
} from '#domain/protocol/messages.ts';
import { PacketType } from '#domain/protocol/PacketType.ts';
import { packetVectors } from '#test/support/vectors.ts';

const SCHEMAS = new Map<number, z.ZodType>([
  [PacketType.Hello, HelloSchema],
  [PacketType.Error, ErrorMessageSchema],
  [PacketType.StartVideo, StartVideoSchema],
  [PacketType.StartAudio, StartAudioSchema],
  [PacketType.VideoConfig, VideoConfigSchema],
  [PacketType.AudioConfig, AudioConfigSchema],
  [PacketType.Status, StatusSchema],
  [PacketType.Log, LogSchema],
]);

describe('JSON messages against golden vectors', () => {
  for (const vector of packetVectors().valid) {
    if (vector.payload.kind !== 'json') continue;
    const { text, value } = vector.payload;
    const schema = SCHEMAS.get(vector.header.type);

    it(`"${vector.name}" is canonical and matches its schema`, () => {
      assert.equal(canonicalJson(value as JsonValue), text);
      assert.ok(schema, `no schema for type ${vector.header.type}`);
      const parsed = parseJsonPayload(schema, Buffer.from(text, 'utf8'), vector.name);
      assert.equal(canonicalJson(parsed as JsonValue), text, 'parse must not lose or add fields');
    });
  }
});

describe('parseJsonPayload', () => {
  it('ignores unknown keys (forward compatibility)', () => {
    const payload = Buffer.from('{"futureField":1,"processing":"raw"}');
    assert.deepEqual(parseJsonPayload(StartAudioSchema, payload, 'StartAudio'), { processing: 'raw' });
  });

  it('reports invalid JSON as a BAD_MESSAGE violation', () => {
    assert.throws(
      () => parseJsonPayload(StartAudioSchema, Buffer.from('{nope'), 'StartAudio'),
      (error: unknown) => error instanceof ProtocolError && error.violation === 'BAD_MESSAGE',
    );
  });

  it('reports schema violations as BAD_MESSAGE', () => {
    assert.throws(
      () => parseJsonPayload(StartAudioSchema, Buffer.from('{"processing":"loud"}'), 'StartAudio'),
      (error: unknown) => error instanceof ProtocolError && error.violation === 'BAD_MESSAGE',
    );
  });
});

describe('canonicalJson', () => {
  it('sorts keys by code point and drops undefined members', () => {
    assert.equal(canonicalJson({ b: 1, a: { d: true, c: null }, e: undefined }), '{"a":{"c":null,"d":true},"b":1}');
  });

  it('orders uppercase before lowercase (code point, not locale)', () => {
    assert.equal(canonicalJson({ a: 1, B: 2 }), '{"B":2,"a":1}');
  });

  it('rejects non-integers', () => {
    assert.throws(() => canonicalJson({ level: 0.5 }), TypeError);
  });

  it('does not escape forward slashes', () => {
    assert.equal(canonicalJson({ path: 'a/b' }), '{"path":"a/b"}');
  });
});

#!/usr/bin/env node
// Generates protocol/test-vectors/*.json.
//
// Bytes are assembled by hand with DataView so the vectors stay independent of every codec they
// are used to test (TypeScript host, Swift BridgeKit). Run: node protocol/tools/generate-test-vectors.mjs

import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const outDir = join(dirname(fileURLToPath(import.meta.url)), '..', 'test-vectors');
mkdirSync(outDir, { recursive: true });

const MAGIC = 0x4d574252; // "MWBR"
const HEADER_SIZE = 24;

const hex = (bytes) => Buffer.from(bytes).toString('hex');
const fromHex = (text) => Buffer.from(text.replace(/\s+/g, ''), 'hex');

/** Orders strings by Unicode code point, as protocol/SPEC.md §4 requires. */
function compareCodePoints(a, b) {
  const left = Array.from(a, (c) => c.codePointAt(0));
  const right = Array.from(b, (c) => c.codePointAt(0));
  for (let i = 0; i < Math.min(left.length, right.length); i++) {
    if (left[i] !== right[i]) return left[i] - right[i];
  }
  return left.length - right.length;
}

/** Canonical JSON: sorted keys (code point order), no whitespace, integers only. */
function canonicalJson(value) {
  if (Array.isArray(value)) return `[${value.map(canonicalJson).join(',')}]`;
  if (value !== null && typeof value === 'object') {
    const keys = Object.keys(value).sort(compareCodePoints);
    return `{${keys.map((k) => `${JSON.stringify(k)}:${canonicalJson(value[k])}`).join(',')}}`;
  }
  if (typeof value === 'number' && !Number.isInteger(value)) throw new Error(`non-integer ${value}`);
  return JSON.stringify(value);
}

function header({ magic = MAGIC, type, flags = 0, seq = 0, payloadLength, timestampUs = 0n }) {
  const buffer = new ArrayBuffer(HEADER_SIZE);
  const view = new DataView(buffer);
  view.setUint32(0, magic);
  view.setUint8(4, type);
  view.setUint8(5, flags);
  view.setUint16(6, 0);
  view.setUint32(8, seq);
  view.setUint32(12, payloadLength);
  view.setBigInt64(16, BigInt(timestampUs));
  return new Uint8Array(buffer);
}

function packet(fields, payload = new Uint8Array(0)) {
  return Buffer.concat([header({ ...fields, payloadLength: payload.length }), payload]);
}

function jsonPayload(value) {
  const text = canonicalJson(value);
  return { text, bytes: Buffer.from(text, 'utf8') };
}

function int16le(samples) {
  const out = Buffer.alloc(samples.length * 2);
  samples.forEach((s, i) => out.writeInt16LE(s, i * 2));
  return out;
}

function pongPayload(echoT0, t1) {
  const out = Buffer.alloc(16);
  out.writeBigInt64BE(BigInt(echoT0), 0);
  out.writeBigInt64BE(BigInt(t1), 8);
  return out;
}

// ---------------------------------------------------------------------------------------------
// packets.json
// ---------------------------------------------------------------------------------------------

const valid = [];
function addValid(name, fields, payloadSpec) {
  let payloadBytes;
  let payload;
  switch (payloadSpec.kind) {
    case 'none':
      payloadBytes = new Uint8Array(0);
      payload = { kind: 'none' };
      break;
    case 'json': {
      const { text, bytes } = jsonPayload(payloadSpec.value);
      payloadBytes = bytes;
      payload = { kind: 'json', text, value: payloadSpec.value };
      break;
    }
    case 'pong':
      payloadBytes = pongPayload(payloadSpec.echoT0, payloadSpec.t1);
      payload = { kind: 'pong', echoT0: String(payloadSpec.echoT0), t1: String(payloadSpec.t1) };
      break;
    case 'hex':
      payloadBytes = fromHex(payloadSpec.hex);
      payload = { kind: 'hex', hex: hex(payloadBytes) };
      break;
    default:
      throw new Error(payloadSpec.kind);
  }
  const bytes = packet(fields, payloadBytes);
  valid.push({
    name,
    hex: hex(bytes),
    header: {
      type: fields.type,
      flags: fields.flags ?? 0,
      seq: fields.seq ?? 0,
      payloadLength: payloadBytes.length,
      timestampUs: String(fields.timestampUs ?? 0n),
    },
    payload,
  });
  return bytes;
}

const ping = addValid('ping', { type: 0x03, timestampUs: 1_000_000n }, { kind: 'none' });
addValid('pong', { type: 0x04, timestampUs: 2_000_700n }, { kind: 'pong', echoT0: 1_000_000n, t1: 2_000_500n });
addValid('hello-host', { type: 0x01 }, {
  kind: 'json',
  value: {
    protocol: { major: 1, minor: 0 },
    role: 'host',
    app: { name: 'mobile-webcam-bridge-host', version: '0.1.0', build: 'dev', gitSha: '0000000' },
    features: ['audio.pcm', 'log', 'status', 'video.h264'],
  },
});
addValid('hello-device', { type: 0x01, timestampUs: 42n }, {
  kind: 'json',
  value: {
    protocol: { major: 1, minor: 0 },
    role: 'device',
    app: { name: 'mobile-webcam-bridge-ios', version: '0.1.0', build: '42', gitSha: '0123abc' },
    device: { model: 'iPhone16,1', name: 'iPhone', os: 'iOS 26.0' },
    features: ['audio.pcm', 'log', 'status', 'video.h264'],
  },
});
addValid('hello-device-android', { type: 0x01, timestampUs: 43n }, {
  kind: 'json',
  value: {
    protocol: { major: 1, minor: 0 },
    role: 'device',
    app: { name: 'mobile-webcam-bridge-android', version: '0.1.0', build: '7', gitSha: '0123abc' },
    device: { model: 'Pixel 9', name: 'Pixel', os: 'Android 16' },
    features: ['audio.pcm', 'log', 'status', 'video.h264'],
  },
});
addValid('start-video', { type: 0x10, seq: 1, timestampUs: 5_000n }, {
  kind: 'json',
  value: { width: 1280, height: 720, fps: 30, bitrateKbps: 6000, camera: 'back.wide', mirror: false, orientation: 'auto', encoder: 'lowLatency' },
});
addValid('start-audio', { type: 0x13, timestampUs: 5_001n }, { kind: 'json', value: { processing: 'standard' } });
addValid('stop-video-max-seq', { type: 0x11, seq: 0xffffffff, timestampUs: 7n }, { kind: 'none' });
addValid('request-keyframe', { type: 0x12, seq: 3 }, { kind: 'none' });
addValid('video-config', { type: 0x20, timestampUs: 9_000n }, {
  kind: 'json',
  value: {
    configId: 1, codec: 'h264', profile: 'constrainedHigh', width: 1280, height: 720, fps: 30,
    bitrateKbps: 6000, rotationDeg: 0, mirrored: false, encoder: 'lowLatency', camera: 'back.wide',
  },
});
const idr = addValid('video-access-unit-idr', { type: 0x21, flags: 0x03, seq: 5, timestampUs: 33_333n }, {
  kind: 'hex',
  hex: '00000001 6742c01f8c8d40 00000001 68ce3c80 00000001 65888400 33ff',
});
addValid('audio-config', { type: 0x30 }, { kind: 'json', value: { configId: 1, format: 's16le', sampleRate: 48000, channels: 1 } });
const audio = addValid('audio-chunk-discontinuity', { type: 0x31, flags: 0x01, seq: 9, timestampUs: 123_456_789n }, {
  kind: 'hex',
  hex: hex(int16le([0, 1, -1, 32767, -32768])),
});
addValid('status', { type: 0x40, seq: 2, timestampUs: 10_000_000n }, {
  kind: 'json',
  value: {
    appState: 'active',
    audio: { state: 'running' },
    battery: { levelPercent: 81, charging: true },
    lowPower: false,
    permissions: { camera: 'authorized', microphone: 'authorized' },
    thermal: 'nominal',
    video: { state: 'running', width: 1280, height: 720, fps: 30, bitrateKbps: 6000 },
  },
});
addValid('log', { type: 0x41, seq: 11, timestampUs: 10_000_001n }, {
  kind: 'json',
  value: { level: 'info', category: 'capture', message: 'Session started' },
});
addValid('error', { type: 0x02 }, {
  kind: 'json',
  value: { code: 'VERSION_MISMATCH', message: 'Unsupported protocol major version 2', fatal: true },
});
addValid('negative-timestamp', { type: 0x03, timestampUs: -5n }, { kind: 'none' });

const invalid = [
  { name: 'bad-magic', hex: hex(packet({ magic: 0x4d574253, type: 0x03 })), error: 'BAD_MAGIC' },
  { name: 'audio-too-large', hex: hex(header({ type: 0x31, payloadLength: 65_537 })), error: 'PAYLOAD_TOO_LARGE' },
  { name: 'video-too-large', hex: hex(header({ type: 0x21, payloadLength: 4_194_305 })), error: 'PAYLOAD_TOO_LARGE' },
  { name: 'json-too-large', hex: hex(header({ type: 0x01, payloadLength: 65_537 })), error: 'PAYLOAD_TOO_LARGE' },
  { name: 'log-too-large', hex: hex(header({ type: 0x41, payloadLength: 8_193 })), error: 'PAYLOAD_TOO_LARGE' },
  { name: 'unknown-type-too-large', hex: hex(header({ type: 0x7f, payloadLength: 4_194_305 })), error: 'PAYLOAD_TOO_LARGE' },
  { name: 'ping-with-payload', hex: hex(packet({ type: 0x03 }, fromHex('0000'))), error: 'BAD_PAYLOAD_LENGTH' },
  { name: 'pong-wrong-length', hex: hex(packet({ type: 0x04 }, fromHex('0000000000000000'))), error: 'BAD_PAYLOAD_LENGTH' },
];

const unknown = packet({ type: 0x7f, seq: 4, timestampUs: 1n }, fromHex('010203'));
const streams = [
  {
    name: 'three-packets',
    hex: hex(Buffer.concat([ping, audio, idr])),
    expect: [
      { type: 0x03, flags: 0, seq: 0, payloadHex: '' },
      { type: 0x31, flags: 0x01, seq: 9, payloadHex: hex(int16le([0, 1, -1, 32767, -32768])) },
      { type: 0x21, flags: 0x03, seq: 5, payloadHex: hex(idr.subarray(HEADER_SIZE)) },
    ],
  },
  {
    name: 'unknown-type-is-delivered-raw',
    hex: hex(Buffer.concat([unknown, ping])),
    expect: [
      { type: 0x7f, flags: 0, seq: 4, payloadHex: '010203' },
      { type: 0x03, flags: 0, seq: 0, payloadHex: '' },
    ],
  },
];

writeFileSync(
  join(outDir, 'packets.json'),
  `${JSON.stringify({ version: 1, headerSize: HEADER_SIZE, valid, invalid, streams }, null, 2)}\n`,
);

// ---------------------------------------------------------------------------------------------
// h264.json
// ---------------------------------------------------------------------------------------------

const sps = '6742c01f8c8d40';
const pps = '68ce3c80';
const idrSlice = '65888400 33ff';
const pSliceWithEmulation = '419a0000030000030180'; // ends with the rbsp stop bit, as every NAL does

const h264 = {
  version: 1,
  accessUnitDelimiter: '0000000109f0',
  split: [
    {
      name: 'idr-with-parameter-sets',
      annexB: hex(fromHex(`00000001 ${sps} 00000001 ${pps} 00000001 ${idrSlice}`)),
      nals: [
        { type: 7, hex: sps },
        { type: 8, hex: pps },
        { type: 5, hex: hex(fromHex(idrSlice)) },
      ],
      isIdr: true,
      hasParameterSets: true,
    },
    {
      name: 'three-byte-start-codes-and-trailing-zero',
      annexB: hex(fromHex(`000001 ${pSliceWithEmulation} 00 000001 0605ff`)),
      nals: [
        { type: 1, hex: pSliceWithEmulation },
        { type: 6, hex: '0605ff' },
      ],
      isIdr: false,
      hasParameterSets: false,
    },
  ],
  avccToAnnexB: [
    {
      name: 'two-nals-length-prefixed',
      avcc: hex(fromHex('00000004 419a0203 00000002 0605')),
      annexB: hex(fromHex('00000001 419a0203 00000001 0605')),
    },
    {
      name: 'idr-with-prepended-parameter-sets',
      sps,
      pps,
      avcc: hex(fromHex(`00000006 ${idrSlice}`)),
      annexB: hex(fromHex(`00000001 ${sps} 00000001 ${pps} 00000001 ${idrSlice}`)),
    },
  ],
};

writeFileSync(join(outDir, 'h264.json'), `${JSON.stringify(h264, null, 2)}\n`);

// ---------------------------------------------------------------------------------------------
// usbmux.json (host only)
// ---------------------------------------------------------------------------------------------

function usbmuxHeader(payloadLength, tag) {
  const out = Buffer.alloc(16);
  out.writeUInt32LE(16 + payloadLength, 0);
  out.writeUInt32LE(1, 4); // version: plist
  out.writeUInt32LE(8, 8); // message: plist
  out.writeUInt32LE(tag, 12);
  return out;
}

const plistDoc = (body) =>
  `<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0">\n${body}\n</plist>\n`;

const resultXml = Buffer.from(plistDoc('<dict>\n\t<key>MessageType</key>\n\t<string>Result</string>\n\t<key>Number</key>\n\t<integer>0</integer>\n</dict>'));
const tunnelBytes = Buffer.concat([ping.subarray(0, 10)]);
const attachedXml = Buffer.from(
  plistDoc(
    '<dict>\n\t<key>DeviceID</key>\n\t<integer>3</integer>\n\t<key>MessageType</key>\n\t<string>Attached</string>\n' +
      '\t<key>Properties</key>\n\t<dict>\n\t\t<key>ConnectionSpeed</key>\n\t\t<integer>480000000</integer>\n' +
      '\t\t<key>ConnectionType</key>\n\t\t<string>USB</string>\n\t\t<key>DeviceID</key>\n\t\t<integer>3</integer>\n' +
      '\t\t<key>LocationID</key>\n\t\t<integer>0</integer>\n\t\t<key>ProductID</key>\n\t\t<integer>4776</integer>\n' +
      '\t\t<key>SerialNumber</key>\n\t\t<string>00008110-001A2B3C4D5E6F70</string>\n\t</dict>\n</dict>',
  ),
);
const detachedXml = Buffer.from(
  plistDoc('<dict>\n\t<key>DeviceID</key>\n\t<integer>3</integer>\n\t<key>MessageType</key>\n\t<string>Detached</string>\n</dict>'),
);

const usbmux = {
  version: 1,
  portNumber: { port: 27100, field: 56425 },
  header: { payloadLength: 100, tag: 7, hex: hex(usbmuxHeader(100, 7)) },
  resultWithTunnelBytes: {
    hex: hex(Buffer.concat([usbmuxHeader(resultXml.length, 2), resultXml, tunnelBytes])),
    tag: 2,
    message: { MessageType: 'Result', Number: 0 },
    leftoverHex: hex(tunnelBytes),
  },
  attached: {
    hex: hex(Buffer.concat([usbmuxHeader(attachedXml.length, 0), attachedXml])),
    device: { deviceId: 3, udid: '00008110-001A2B3C4D5E6F70', connectionType: 'USB', productId: 4776 },
  },
  detached: {
    hex: hex(Buffer.concat([usbmuxHeader(detachedXml.length, 0), detachedXml])),
    deviceId: 3,
  },
};

writeFileSync(join(outDir, 'usbmux.json'), `${JSON.stringify(usbmux, null, 2)}\n`);

console.log(`wrote packets.json (${valid.length} valid, ${invalid.length} invalid, ${streams.length} streams), h264.json, usbmux.json`);

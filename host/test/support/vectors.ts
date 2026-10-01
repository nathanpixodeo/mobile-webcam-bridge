import { readFileSync } from 'node:fs';

const vectorsDir = new URL('../../../protocol/test-vectors/', import.meta.url);

function load(name: string): unknown {
  return JSON.parse(readFileSync(new URL(name, vectorsDir), 'utf8'));
}

export interface VectorHeader {
  readonly type: number;
  readonly flags: number;
  readonly seq: number;
  readonly payloadLength: number;
  readonly timestampUs: string;
}

export type VectorPayload =
  | { readonly kind: 'none' }
  | { readonly kind: 'json'; readonly text: string; readonly value: unknown }
  | { readonly kind: 'pong'; readonly echoT0: string; readonly t1: string }
  | { readonly kind: 'hex'; readonly hex: string };

export interface PacketVectors {
  readonly version: number;
  readonly headerSize: number;
  readonly valid: readonly {
    readonly name: string;
    readonly hex: string;
    readonly header: VectorHeader;
    readonly payload: VectorPayload;
  }[];
  readonly invalid: readonly { readonly name: string; readonly hex: string; readonly error: string }[];
  readonly streams: readonly {
    readonly name: string;
    readonly hex: string;
    readonly expect: readonly {
      readonly type: number;
      readonly flags: number;
      readonly seq: number;
      readonly payloadHex: string;
    }[];
  }[];
}

export interface H264Vectors {
  readonly accessUnitDelimiter: string;
  readonly split: readonly {
    readonly name: string;
    readonly annexB: string;
    readonly nals: readonly { readonly type: number; readonly hex: string }[];
    readonly isIdr: boolean;
    readonly hasParameterSets: boolean;
  }[];
}

export interface UsbmuxVectors {
  readonly portNumber: { readonly port: number; readonly field: number };
  readonly header: { readonly payloadLength: number; readonly tag: number; readonly hex: string };
  readonly resultWithTunnelBytes: {
    readonly hex: string;
    readonly tag: number;
    readonly message: Record<string, unknown>;
    readonly leftoverHex: string;
  };
  readonly attached: {
    readonly hex: string;
    readonly device: {
      readonly deviceId: number;
      readonly udid: string;
      readonly connectionType: string;
      readonly productId: number;
    };
  };
  readonly detached: { readonly hex: string; readonly deviceId: number };
}

export const packetVectors = (): PacketVectors => load('packets.json') as PacketVectors;
export const h264Vectors = (): H264Vectors => load('h264.json') as H264Vectors;
export const usbmuxVectors = (): UsbmuxVectors => load('usbmux.json') as UsbmuxVectors;

export const hex = (text: string): Buffer => Buffer.from(text, 'hex');

/** Small deterministic PRNG (mulberry32) for reproducible fuzzing. */
export function seededRandom(seed: number): () => number {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6d2b79f5) >>> 0;
    let t = state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

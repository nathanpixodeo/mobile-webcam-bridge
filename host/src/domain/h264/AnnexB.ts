/** H.264 Annex-B byte-stream helpers (ITU-T H.264 Annex B). */

export const NalType = {
  NonIdrSlice: 1,
  IdrSlice: 5,
  Sei: 6,
  Sps: 7,
  Pps: 8,
  AccessUnitDelimiter: 9,
} as const;

/**
 * Access unit delimiter NAL with `primary_pic_type = 7` (any slice type). Appending it after an
 * access unit lets ffmpeg's parser emit that frame immediately instead of waiting for the next
 * access unit to begin, which removes one frame of decode latency.
 */
export const ACCESS_UNIT_DELIMITER: Buffer = Buffer.from([0x00, 0x00, 0x00, 0x01, 0x09, 0xf0]);

export function nalUnitType(nal: Uint8Array): number {
  return (nal[0] ?? 0) & 0x1f;
}

/**
 * Splits an Annex-B stream into NAL units (start codes removed, trailing zero bytes trimmed).
 * Accepts both 3- and 4-byte start codes. Returned arrays are views into `stream`.
 */
export function splitNalUnits(stream: Uint8Array): Uint8Array[] {
  const nals: Uint8Array[] = [];
  let nalStart = -1;
  let i = 0;
  while (i + 2 < stream.length) {
    if (stream[i] === 0 && stream[i + 1] === 0 && stream[i + 2] === 1) {
      if (nalStart >= 0) pushTrimmed(nals, stream, nalStart, i);
      i += 3;
      nalStart = i;
    } else {
      i++;
    }
  }
  if (nalStart >= 0) pushTrimmed(nals, stream, nalStart, stream.length);
  return nals;
}

function pushTrimmed(out: Uint8Array[], stream: Uint8Array, start: number, end: number): void {
  let last = end;
  while (last > start && stream[last - 1] === 0) last--;
  if (last > start) out.push(stream.subarray(start, last));
}

export interface AccessUnitInfo {
  readonly nalTypes: readonly number[];
  readonly isIdr: boolean;
  readonly hasParameterSets: boolean;
}

export function describeAccessUnit(accessUnit: Uint8Array): AccessUnitInfo {
  const nalTypes = splitNalUnits(accessUnit).map(nalUnitType);
  return {
    nalTypes,
    isIdr: nalTypes.includes(NalType.IdrSlice),
    hasParameterSets: nalTypes.includes(NalType.Sps) && nalTypes.includes(NalType.Pps),
  };
}

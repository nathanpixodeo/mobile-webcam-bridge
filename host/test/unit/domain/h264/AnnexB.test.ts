import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { ACCESS_UNIT_DELIMITER, describeAccessUnit, nalUnitType, splitNalUnits } from '#domain/h264/AnnexB.ts';
import { KeyframeGate } from '#domain/h264/KeyframeGate.ts';
import { h264Vectors, hex } from '#test/support/vectors.ts';

const vectors = h264Vectors();

describe('AnnexB', () => {
  it('exposes the access unit delimiter from the vectors', () => {
    assert.equal(ACCESS_UNIT_DELIMITER.toString('hex'), vectors.accessUnitDelimiter);
    const [aud] = splitNalUnits(ACCESS_UNIT_DELIMITER);
    assert.ok(aud);
    assert.equal(nalUnitType(aud), 9);
  });

  for (const vector of vectors.split) {
    it(`splits "${vector.name}"`, () => {
      const nals = splitNalUnits(hex(vector.annexB));
      assert.deepEqual(
        nals.map((nal) => ({ type: nalUnitType(nal), hex: Buffer.from(nal).toString('hex') })),
        vector.nals,
      );
      const info = describeAccessUnit(hex(vector.annexB));
      assert.equal(info.isIdr, vector.isIdr);
      assert.equal(info.hasParameterSets, vector.hasParameterSets);
    });
  }

  it('returns nothing for data without a start code', () => {
    assert.deepEqual(splitNalUnits(Buffer.from([1, 2, 3, 4])), []);
  });
});

describe('KeyframeGate', () => {
  it('drops access units until the first IDR', () => {
    const gate = new KeyframeGate();
    assert.equal(gate.admit({ isIdr: false }), false);
    assert.equal(gate.admit({ isIdr: true }), true);
    assert.equal(gate.admit({ isIdr: false }), true);
  });

  it('waits for the next IDR after a loss', () => {
    const gate = new KeyframeGate();
    gate.admit({ isIdr: true });
    gate.markLoss();
    assert.equal(gate.isWaiting, true);
    assert.equal(gate.admit({ isIdr: false }), false);
    assert.equal(gate.admit({ isIdr: true }), true);
  });
});

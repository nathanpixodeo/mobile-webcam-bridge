/**
 * FIFO of byte chunks with cheap appends and reads that only copy when a read spans chunks.
 * Avoids the quadratic cost of repeatedly concatenating socket chunks.
 */
export class ByteQueue {
  readonly #chunks: Buffer[] = [];
  #headOffset = 0;
  #length = 0;

  get length(): number {
    return this.#length;
  }

  append(chunk: Buffer): void {
    if (chunk.length === 0) return;
    this.#chunks.push(chunk);
    this.#length += chunk.length;
  }

  /** Copies the first `count` bytes into a new buffer without consuming them. */
  peek(count: number): Buffer {
    const out = Buffer.allocUnsafe(count);
    this.#copyInto(out, count, false);
    return out;
  }

  /**
   * Removes and returns the first `count` bytes. Returns a zero-copy view when the bytes lie in
   * a single chunk, otherwise a fresh buffer.
   */
  take(count: number): Buffer {
    if (count > this.#length) throw new RangeError(`take(${count}) exceeds queued ${this.#length} bytes`);
    const head = this.#chunks[0];
    if (head !== undefined && head.length - this.#headOffset >= count) {
      const view = head.subarray(this.#headOffset, this.#headOffset + count);
      this.#advance(count);
      return view;
    }
    const out = Buffer.allocUnsafe(count);
    this.#copyInto(out, count, true);
    return out;
  }

  /** Removes `count` bytes without returning them. */
  skip(count: number): void {
    if (count > this.#length) throw new RangeError(`skip(${count}) exceeds queued ${this.#length} bytes`);
    this.#advance(count);
  }

  clear(): void {
    this.#chunks.length = 0;
    this.#headOffset = 0;
    this.#length = 0;
  }

  #copyInto(target: Buffer, count: number, consume: boolean): void {
    let written = 0;
    let offset = this.#headOffset;
    for (const chunk of this.#chunks) {
      if (written === count) break;
      const available = chunk.length - offset;
      const n = Math.min(available, count - written);
      chunk.copy(target, written, offset, offset + n);
      written += n;
      offset = 0;
    }
    if (consume) this.#advance(count);
  }

  #advance(count: number): void {
    let remaining = count;
    while (remaining > 0) {
      const head = this.#chunks[0];
      if (head === undefined) break;
      const available = head.length - this.#headOffset;
      if (remaining < available) {
        this.#headOffset += remaining;
        break;
      }
      remaining -= available;
      this.#chunks.shift();
      this.#headOffset = 0;
    }
    this.#length -= count;
  }
}

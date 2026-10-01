import { open, type FileHandle } from 'node:fs/promises';

const HEADER_SIZE = 44;

/** Streams 16-bit PCM into a WAV file; the RIFF sizes are patched on close. */
export class WavWriter implements AsyncDisposable {
  readonly #file: FileHandle;
  readonly #sampleRate: number;
  readonly #channels: number;
  #dataBytes = 0;
  #closed = false;

  private constructor(file: FileHandle, sampleRate: number, channels: number) {
    this.#file = file;
    this.#sampleRate = sampleRate;
    this.#channels = channels;
  }

  static async create(path: string, sampleRate: number, channels: number): Promise<WavWriter> {
    const file = await open(path, 'w');
    const writer = new WavWriter(file, sampleRate, channels);
    await file.write(writer.#header(0), 0, HEADER_SIZE, 0);
    return writer;
  }

  get samplesWritten(): number {
    return this.#dataBytes / 2 / this.#channels;
  }

  async write(samples: Int16Array): Promise<void> {
    const bytes = Buffer.from(samples.buffer, samples.byteOffset, samples.byteLength);
    await this.#file.write(bytes, 0, bytes.length, HEADER_SIZE + this.#dataBytes);
    this.#dataBytes += bytes.length;
  }

  async [Symbol.asyncDispose](): Promise<void> {
    if (this.#closed) return;
    this.#closed = true;
    await this.#file.write(this.#header(this.#dataBytes), 0, HEADER_SIZE, 0);
    await this.#file.close();
  }

  #header(dataBytes: number): Buffer {
    const header = Buffer.alloc(HEADER_SIZE);
    const blockAlign = this.#channels * 2;
    header.write('RIFF', 0, 'ascii');
    header.writeUInt32LE(36 + dataBytes, 4);
    header.write('WAVE', 8, 'ascii');
    header.write('fmt ', 12, 'ascii');
    header.writeUInt32LE(16, 16);
    header.writeUInt16LE(1, 20); // PCM
    header.writeUInt16LE(this.#channels, 22);
    header.writeUInt32LE(this.#sampleRate, 24);
    header.writeUInt32LE(this.#sampleRate * blockAlign, 28);
    header.writeUInt16LE(blockAlign, 32);
    header.writeUInt16LE(16, 34);
    header.write('data', 36, 'ascii');
    header.writeUInt32LE(dataBytes, 40);
    return header;
  }
}

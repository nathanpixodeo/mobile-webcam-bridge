import type { DoctorCheckResult } from '#ports/NativeHelper.ts';

/** Console output for humans (commands); structured logs go through the logger instead. */
export class Terminal {
  readonly #color: boolean;

  constructor(color = process.stdout.isTTY) {
    this.#color = color;
  }

  line(text = ''): void {
    console.log(text);
  }

  heading(text: string): void {
    this.line(this.#paint(text, '1'));
  }

  error(text: string): void {
    console.error(this.#paint(text, '31'));
  }

  /** Prints rows as aligned columns. */
  table(headers: readonly string[], rows: readonly (readonly string[])[]): void {
    const widths = headers.map((header, column) =>
      Math.max(header.length, ...rows.map((row) => (row[column] ?? '').length)),
    );
    const format = (cells: readonly string[]): string =>
      cells
        .map((cell, i) => cell.padEnd(widths[i] ?? 0))
        .join('  ')
        .trimEnd();
    this.line(this.#paint(format(headers), '1'));
    for (const row of rows) this.line(format(row));
  }

  checks(results: readonly DoctorCheckResult[]): void {
    for (const result of results) {
      const symbol =
        result.status === 'pass'
          ? this.#paint('✔', '32')
          : result.status === 'warn'
            ? this.#paint('!', '33')
            : this.#paint('✖', '31');
      this.line(`${symbol} ${result.id.padEnd(28)} ${result.message}`);
    }
  }

  #paint(text: string, code: string): string {
    return this.#color ? `\u001b[${code}m${text}\u001b[0m` : text;
  }
}

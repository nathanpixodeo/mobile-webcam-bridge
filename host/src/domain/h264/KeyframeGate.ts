/**
 * Drops access units until a decodable entry point (an IDR) arrives. A decoder that starts on a
 * P-frame, or resumes after data loss, would only produce corrupt pictures.
 */
export class KeyframeGate {
  #waitingForIdr = true;

  get isWaiting(): boolean {
    return this.#waitingForIdr;
  }

  /** Returns true when the access unit may be passed to the decoder. */
  admit(accessUnit: { readonly isIdr: boolean }): boolean {
    if (accessUnit.isIdr) this.#waitingForIdr = false;
    return !this.#waitingForIdr;
  }

  /** Call after any loss (dropped access unit, decoder restart, reconnect). */
  markLoss(): void {
    this.#waitingForIdr = true;
  }
}

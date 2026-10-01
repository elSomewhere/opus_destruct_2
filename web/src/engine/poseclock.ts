/**
 * When the engine's poses are drawn. They come in batches, one a tick - the rigid pieces'
 * (`debris`), the vehicles' and the characters' together (the worker's pose batch) - and are
 * drawn one batch interval in the past, interpolated between the last two samples: the interval
 * as measured (an engine slowed down sends its ticks further apart: motion stays continuous, only
 * slower), and the same for every kind of pose, so a car's body, its wheels and the camera riding
 * it are drawn at one moment.
 */

/** The engine's tick (s): the interval until batches say otherwise, and the shortest. */
export const TICK_S = 1 / 60;
/** The longest interval measured (s): a longer one is a pause, not the engine's pace. */
const MAX_INTERVAL_S = 0.25;
/** The weight of a new interval in the smoothed one. */
const SMOOTHING = 0.2;

export class PoseClock {
  /** The smoothed interval between batches (s): how far in the past poses are drawn. */
  interval = TICK_S;
  private seq: number | undefined = undefined;
  private last = -1;

  /** A pose message of batch `seq` arrived at `nowS` (a batch's first message counts). */
  batch(seq: number | undefined, nowS: number): void {
    if (seq !== undefined && seq === this.seq) return;
    this.seq = seq;
    if (this.last >= 0) {
      const d = nowS - this.last;
      if (d > 0 && d <= MAX_INTERVAL_S) this.interval += SMOOTHING * (Math.max(TICK_S, d) - this.interval);
    }
    this.last = nowS;
  }

  /** The weight, drawn at `nowS`, of a sample taken at `bt` against the one before it, at `at`. */
  weight(at: number, bt: number, nowS: number): number {
    return Math.min(1, Math.max(0, (nowS - this.interval - at) / Math.max(1e-6, bt - at)));
  }

  /**
   * The sample a new one is interpolated from: the last - after a gap (a resting piece is not
   * sent again, the page fell behind), the last as if one interval ago, so the move starts now.
   */
  from<T extends { t: number }>(last: T, nowS: number): T {
    return nowS - last.t > 2 * this.interval ? { ...last, t: nowS - this.interval } : last;
  }
}

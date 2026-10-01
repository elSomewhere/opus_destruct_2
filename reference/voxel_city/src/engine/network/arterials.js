import { hashFloat } from "../core/hash.js";
import { vx } from "../core/units.js";
import { wrapOf } from "../world/wrap.js";

/**
 * Global arterial lines. Vertical lines x = X(i) and horizontal lines
 * y = Y(j) with per-line jitter; their crossings define the "arterial cells"
 * that are the fundamental, independently planned unit of the city. Because
 * each line depends only on its index, any cell can be planned without its
 * neighbours and all shared edges agree exactly.
 */
export class ArterialGrid {
  constructor(config) {
    this.seed = config.seed;
    this.spacing = vx(config.city.arterialSpacing);
    this.jitter = config.city.arterialJitter;
    // a wrapping world has n lines around it (World.wrap)
    this.wrap = wrapOf(config);
    this.n = this.wrap.count(config.city.arterialSpacing);
  }

  /** Position (voxels, multiple of 8) of vertical line i (axis 0) or horizontal line j (axis 1). */
  line(axis, index) {
    if (this.n) {
      const c = this.wrap.canon(index, this.n);
      return this.lineAt(axis, c) + this.wrap.lap(index, this.n) * this.wrap.sizeV;
    }
    return this.lineAt(axis, index);
  }

  lineAt(axis, index) {
    const j = (hashFloat(this.seed, index, axis === 0 ? 7001 : 7919) - 0.5) * 2 * this.jitter * this.spacing;
    return Math.round((index * this.spacing + j) / 8) * 8;
  }

  /** Canonical line index (a wrapping world's lines repeat every n). */
  canon(index) {
    return this.wrap.canon(index, this.n);
  }

  /** Index of the line at or before coordinate v. */
  indexAt(axis, v) {
    let i = Math.floor(v / this.spacing);
    while (this.line(axis, i) > v) i -= 1;
    while (this.line(axis, i + 1) <= v) i += 1;
    return i;
  }

  /** Cell (i, j) containing the point. */
  cellAt(x, y) {
    return { i: this.indexAt(0, x), j: this.indexAt(1, y) };
  }

  /** Centerline rect of cell (i, j): {x0, y0, x1, y1} (x1/y1 = next line). */
  cellRect(i, j) {
    return { x0: this.line(0, i), y0: this.line(1, j), x1: this.line(0, i + 1), y1: this.line(1, j + 1) };
  }
}

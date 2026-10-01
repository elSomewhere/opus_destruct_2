import { VOXELS_PER_METER } from "../core/units.js";

/**
 * Periodic lattices of a wrapping world (config.world.chart = 'torus'): the
 * world repeats every `size` meters, so a lattice with `n` cells around the
 * world hashes the canonical index `mod(i, n)` and places cell `i` at its
 * canonical position plus `lap(i) * size`. On unbounded charts every
 * helper is the identity, so planar worlds are unchanged.
 *
 * Positions stay unwrapped everywhere (the plane is self-consistent and has
 * no seam); only the seeds are canonical, so the content of every lap is
 * the same town, the same streets and the same buildings.
 */
export class Wrap {
  constructor(config) {
    this.on = config.world.chart === "torus";
    /** period in meters and voxels (0 when the world does not wrap) */
    this.size = this.on ? config.world.size : 0;
    this.sizeV = this.size * VOXELS_PER_METER;
  }

  /** Cells of a lattice with the given spacing (m) around the world; 0 = unbounded. */
  count(spacingM) {
    return this.on ? Math.max(1, Math.round(this.size / spacingM)) : 0;
  }

  /** Canonical index of cell i on a lattice of n cells around the world. */
  canon(i, n) {
    return n ? ((i % n) + n) % n : i;
  }

  /** Lap of cell i (how many times around the world), 0 on unbounded charts. */
  lap(i, n) {
    return n ? Math.floor(i / n) : 0;
  }

  /** Canonical coordinate (voxels) for position hashes. */
  v(x) {
    return this.on ? ((x % this.sizeV) + this.sizeV) % this.sizeV : x;
  }

  /** Canonical integer coordinate (voxels) for position hashes (Math.round elsewhere). */
  vi(x) {
    return this.on ? Math.round(this.v(x)) % this.sizeV : Math.round(x);
  }
}

const cache = new WeakMap();

/** The Wrap of a config (cached). */
export function wrapOf(config) {
  let w = cache.get(config);
  if (!w) {
    w = new Wrap(config);
    cache.set(config, w);
  }
  return w;
}

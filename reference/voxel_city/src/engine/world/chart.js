/**
 * Charts map 2D world-plane coordinates (meters) to the 3D domain used by
 * macro noise fields.
 *
 * All local generation (roads, buildings, interiors, voxels) happens in a flat
 * chart frame with z up. For a spherical planet, a cube-sphere chart maps each
 * face's (x, y) to a point on the sphere so biome / climate / settlement
 * fields stay continuous across faces, while the local voxel grid of each face
 * remains flat (curvature is negligible at city scale).
 *
 * A planet world streams one face at a time: generation runs in the face's
 * flat coordinates exactly as on the plane, while climate (with latitude),
 * urbanization, biomes and terrain are sampled on the sphere, so they line
 * up with the neighbouring faces. Stitching the local structures (roads,
 * lots) across face edges is the remaining step for a seamless planet.
 */
export class FlatChart {
  constructor() {
    this.id = "flat";
  }

  /** 2D chart coords (m) -> 3D field-sampling coords (m) */
  toField(x, y) {
    return [x, y, 0];
  }

  /** Charts are unbounded unless they say otherwise. */
  contains() {
    return true;
  }

  /** Distance (m) from a point to the chart edge: unbounded. */
  edgeDistance() {
    return Infinity;
  }
}

/**
 * One face of a cube-sphere planet. Local coords span [-half, half] on the
 * face; field coords are the projected point on a sphere of `radius` meters.
 */
export class CubeSphereChart {
  constructor({ radius, face }) {
    this.id = `cube:${face}`;
    this.radius = radius;
    this.face = face;
    this.half = (Math.PI / 4) * radius;
  }

  toField(x, y) {
    const a = x / this.half;
    const b = y / this.half;
    let px;
    let py;
    let pz;
    switch (this.face) {
      case 0: px = 1; py = a; pz = b; break;
      case 1: px = -1; py = -a; pz = b; break;
      case 2: px = -a; py = 1; pz = b; break;
      case 3: px = a; py = -1; pz = b; break;
      case 4: px = a; py = b; pz = 1; break;
      default: px = a; py = -b; pz = -1; break;
    }
    const len = Math.hypot(px, py, pz);
    return [(px / len) * this.radius, (py / len) * this.radius, (pz / len) * this.radius];
  }

  contains(x, y) {
    return Math.abs(x) <= this.half && Math.abs(y) <= this.half;
  }

  /** Distance (m) from a face point to the nearest face edge (negative outside). */
  edgeDistance(x, y) {
    return this.half - Math.max(Math.abs(x), Math.abs(y));
  }

  /** Latitude in -1..1 (sine) of a field point: z is the planet axis. */
  latitude(fx, fy, fz) {
    return fz / this.radius;
  }
}

/**
 * A flat world that wraps around: the plane repeats every `size` meters in
 * x and in y (a torus, like a flat planet). Field coordinates are the
 * point on a Clifford torus in 4D, [R cos a, R sin a, R cos b, R sin b]
 * with R = size / 2π: that embedding is isometric, so noise sampled there
 * keeps its scale everywhere and every field built on the chart is
 * seamlessly periodic (noise.js nP / fbmP / ridgedP take the 4th
 * coordinate).
 *
 * The world stays an unbounded plane for everything else (streaming, the
 * viewer): lattices (arterials, settlements, highways, sites, lakes,
 * forests) get an integer number of cells around the world and hash their
 * canonical index (World.wrap), so walking `size` meters east brings you
 * back to the same town, the same streets and the same buildings.
 */
export class TorusChart {
  constructor({ size, latitude = true }) {
    this.id = `torus:${size}`;
    this.size = size;
    this.R = size / (2 * Math.PI);
    this.hasLatitude = latitude;
    this.memo = { x: NaN, y: NaN, p: null };
  }

  toField(x, y) {
    const m = this.memo;
    if (x === m.x && y === m.y) return m.p;
    const S = this.size;
    // canonical position first: x and x + size give bit-identical fields
    const a = ((((x % S) + S) % S) / S) * 2 * Math.PI;
    const b = ((((y % S) + S) % S) / S) * 2 * Math.PI;
    const R = this.R;
    const p = [R * Math.cos(a), R * Math.sin(a), R * Math.cos(b), R * Math.sin(b)];
    m.x = x;
    m.y = y;
    m.p = p;
    return p;
  }

  contains() {
    return true;
  }

  edgeDistance() {
    return Infinity;
  }

  /**
   * Sine of the latitude for climate (0 equator .. ±1 pole): once around the
   * world in y the climate runs from an equator to a pole and back; the
   * origin sits at mid latitudes.
   */
  latitude(fx, fy, fz, fw) {
    if (!this.hasLatitude) return null;
    const b = Math.atan2(fw, fz);
    return Math.sin(b / 2 + Math.PI / 6);
  }
}

/** Chart for a world config: `chart` = 'flat' | 'torus' (uses `size`) | 'cube' (uses `planet`). */
export function makeChart(worldCfg = {}) {
  const kind = typeof worldCfg === "string" ? worldCfg : worldCfg.chart;
  if (kind === "flat" || !kind) return new FlatChart();
  if (kind === "torus") return new TorusChart({ size: worldCfg.size ?? 96000, latitude: worldCfg.latitude ?? true });
  if (kind === "cube") return new CubeSphereChart({ radius: worldCfg.planet?.radius ?? 240000, face: worldCfg.planet?.face ?? 0 });
  throw new Error(`Chart "${kind}" is not implemented`);
}

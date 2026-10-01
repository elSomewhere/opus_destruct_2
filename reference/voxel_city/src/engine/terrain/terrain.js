import { SimplexNoise } from "../core/noise.js";
import { deriveSeed } from "../core/hash.js";
import { smoothstep, lerp } from "../core/math.js";
import { VOXEL_SIZE, VOXELS_PER_METER } from "../core/units.js";
import { desertness } from "../nature/biomes.js";
import { LANDFORMS } from "./landforms.js";

/**
 * Terrain height model.
 *
 *   natural(x,y) : the registered landforms in order (terrain/landforms.js):
 *                  continent, hills, mountain ranges up to ~6 km, desert
 *                  mesas and dunes, canyons, forest ravines
 *   city(x,y)    : settlements sit on graded land at the lowland height of
 *                  their center (weighted where towns meet) plus gentle relief
 *   height(x,y)  : blend of the two by urbanization.
 *
 * Heights are returned in VOXELS (floating point); z = 0 is sea level.
 * Mountainness is a macro field (world/fields.js) so towns can avoid it.
 */
export class Terrain {
  constructor(config, chart, fields) {
    this.config = config;
    this.chart = chart;
    this.fields = fields;
    const seed = config.seed;
    this.nCity = new SimplexNoise(deriveSeed(seed, "terrain.city"));
    this.seaLevel = config.world.seaLevel * VOXELS_PER_METER;
    const noise = (name) => new SimplexNoise(deriveSeed(seed, `terrain.${name}`));
    this.forms = LANDFORMS.all()
      .slice()
      .sort((a, b) => a.order - b.order)
      .map((lf) => ({ lf, st: lf.init(noise) }));
    // reusable per-sample context
    const self = this;
    this.ctx = {
      x: 0,
      y: 0,
      fx: 0,
      fy: 0,
      fz: 0,
      /** 4th field coordinate on a torus chart (undefined elsewhere) and the torus radius */
      fw: undefined,
      torusR: chart.R ?? 0,
      u: 0,
      mountain: 0,
      h: 0,
      lowland: 0,
      ridge: 0,
      canyon: 0,
      ravine: 0,
      /** 0..1 bare rock of a granite outcrop (landforms.js outcrops) */
      outcrop: 0,
      /** 0..1 how close to a stream / ravine / canyon channel (relief keeps off beds) */
      channel: 0,
      _rugged: -1,
      stream: null,
      prox: 0,
      /** island mode: the plan, the coast distance (m, positive inland) and the lazy coast type */
      island: fields.island,
      coast: Infinity,
      _cliff: -1,
      cliff() {
        if (this._cliff < 0) this._cliff = this.island.cliff(this.x * VOXEL_SIZE, this.y * VOXEL_SIZE);
        return this._cliff;
      },
      cfg: config.terrain,
      /** chart point (m) -> field point [fx, fy, fz, fw] (landforms that sample elsewhere, e.g. gullies) */
      toField: (xm, ym) => chart.toField(xm, ym),
      _clim: null,
      _desert: -1,
      climate() {
        if (!this._clim) this._clim = { t: self.fields.temperature(this.x, this.y), m: self.fields.moisture(this.x, this.y) };
        return this._clim;
      },
      desert() {
        if (this._desert < 0) {
          const c = this.climate();
          this._desert = desertness(c.t, c.m);
        }
        return this._desert;
      },
    };
  }

  /** Run the landform stack at voxel (x, y). Leaves hints in this.ctx. */
  natural(x, y, u, fx, fy, fz, prox = null, fw = undefined) {
    const c = this.ctx;
    c.x = x;
    c.y = y;
    c.fx = fx;
    c.fy = fy;
    c.fz = fz;
    c.fw = fw;
    c.u = u;
    c.h = 0;
    c.lowland = 0;
    c.ridge = 0;
    c.canyon = 0;
    c.ravine = 0;
    c.outcrop = 0;
    c.valley = 0;
    c.rough = 0;
    c.channel = 0;
    c._rugged = -1;
    c.stream = null;
    c._clim = null;
    c._desert = -1;
    c._cliff = -1;
    // mountains fade out towards towns (foothills around each one)
    const m = this.fields.mountainness(x, y);
    c.prox = prox ?? this.fields.settlementProximity(x, y);
    c.mountain = m > 0 ? m * (1 - c.prox) : 0;
    if (c.island) {
      // on an island the fells plunge into the sea: steep fjord walls and
      // sea cliffs where the coast is rocky, a long slope down to a beach elsewhere
      c.coast = c.island.coast(x * VOXEL_SIZE, y * VOXEL_SIZE);
      if (c.mountain > 0) c.mountain *= smoothstep(0, 500 + 1100 * (1 - c.cliff()), c.coast);
    }
    for (const { lf, st } of this.forms) lf.apply(c, st);
    return c.h;
  }

  /** Lowland height (m) at a settlement center: its graded city level. */
  settlementBase(s) {
    if (s.baseH === undefined) {
      const [fx, fy, fz, fw] = this.chart.toField(s.x * VOXEL_SIZE, s.y * VOXEL_SIZE);
      this.natural(s.x, s.y, 1, fx, fy, fz, null, fw);
      s.baseH = Math.max(2, this.ctx.lowland);
    }
    return s.baseH;
  }

  cityMeters(fx, fy, fz, fw, ur) {
    const t = this.config.terrain;
    let base = 6;
    if (ur.parts && ur.parts.length) {
      let sw = 0;
      let sb = 0;
      for (const [s, w] of ur.parts) {
        sw += w;
        sb += w * this.settlementBase(s);
      }
      base = sb / sw;
    } else if (ur.settlement) base = this.settlementBase(ur.settlement);
    return base + t.cityRelief * this.nCity.fbmP(fx / t.cityReliefScale, fy / t.cityReliefScale, fz / t.cityReliefScale, fw / t.cityReliefScale, 2);
  }

  /**
   * Full sample: { h (voxels), natural (voxels), u, core, settlement, grade,
   * mountain (0..1), canyon (0..1 depth of a canyon cut), ravine (0..1) }.
   * Pass a precomputed urban sample to avoid re-evaluating it.
   */
  sample(x, y, urban = null, raw = false) {
    const ur = urban ?? this.fields.urban(x, y);
    const [fx, fy, fz, fw] = this.chart.toField(x * VOXEL_SIZE, y * VOXEL_SIZE);
    const nat = this.natural(x, y, ur.u, fx, fy, fz, ur.prox, fw);
    const c = this.ctx;
    const mountain = c.mountain;
    const canyon = c.canyon;
    const ravine = c.ravine;
    const outcrop = c.outcrop;
    const rough = c.rough;
    // a landform's stream channel (canyon floor stream, ravine creek): only in open country
    const stream = c.stream && ur.u < 0.06 ? c.stream : null;
    let w = smoothstep(0.06, 0.32, ur.u);
    let city = w > 0 ? Math.max(this.cityMeters(fx, fy, fz, fw, ur), 2) : 0;
    if (w > 0 && c.island) {
      const rise = c.island.cfg.townRise ?? 0;
      if (rise > 0) {
        // a town climbing the hillside from its harbour (Bergen): a
        // waterfront 2.5 m above the sea rising `townRise` m inland over
        // about its radius, the town's own relief on top
        const t = this.config.terrain;
        const sR = Math.max(250, (ur.settlement?.radius ?? 4000) * VOXEL_SIZE);
        const relief = t.cityRelief * this.nCity.fbmP(fx / t.cityReliefScale, fy / t.cityReliefScale, fz / t.cityReliefScale, fw / t.cityReliefScale, 2);
        city = 2.5 + rise * (1 - Math.exp(-Math.max(0, c.coast) / (sR * 0.85))) + relief * smoothstep(20, 200, c.coast);
      } else {
        // an island town eases down to a waterfront 2.5 m above the sea
        // within 350 m of the shore and never fills the sea beyond it
        const k = smoothstep(0, 350, c.coast);
        city = 2.5 + (city - 2.5) * k;
      }
      w *= smoothstep(-30, 4, c.coast);
    }
    let h = w > 0 ? lerp(nat, city, w) : nat;
    // harbour towns ease down to their lake (world hook; raw samples skip it)
    if (!raw && ur.prox > 0 && this.portGrade) h = this.portGrade(x, y, h);
    return {
      h: h * VOXELS_PER_METER,
      natural: nat * VOXELS_PER_METER,
      u: ur.u,
      core: ur.core,
      settlement: ur.settlement,
      grade: w,
      mountain,
      canyon,
      ravine,
      /** 0..1 bare granite of an outcrop / shore slab (open country only) */
      outcrop: ur.u < 0.08 ? outcrop : 0,
      stream,
      /** metres of small-scale roughness in h (land cover reads slopes without it) */
      rough,
      /** 0..1 how rugged the open country is (landforms.js ruggedness; 0 in towns) */
      rugged: c._rugged > 0 ? c._rugged : 0,
      /** island mode: distance to the shore (m, positive inland); Infinity elsewhere */
      coast: c.coast,
    };
  }

  height(x, y) {
    return this.sample(x, y).h;
  }
}

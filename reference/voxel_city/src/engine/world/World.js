import { makeConfig } from "../config/defaults.js";
import { makeChart } from "./chart.js";
import { MacroFields } from "./fields.js";
import { Terrain } from "../terrain/terrain.js";
import { ArterialGrid } from "../network/arterials.js";
import { LRU } from "../core/lru.js";
import { planCellNetwork } from "../city/cellNetwork.js";
import { planCell } from "../city/cellPlan.js";
import { RoadView } from "../network/roadView.js";
import "../city/districts.js";

/**
 * The World is the single entry point to generation. Everything is lazy and
 * cached by structural key:
 *
 *   fields / terrain         pointwise, no cache needed
 *   arterial grid            pure functions of line index
 *   cell network (stage 1)   roads + districts + blocks per arterial cell
 *   road view                junction-annotated roads of a 3x3 neighbourhood
 *   cell plan (stage 2)      lots, building envelopes, open spaces
 *   building plan            full interior, generated on first LOD0 request
 *
 * Every product is a pure function of (config, key): caches can be dropped
 * at any time and any worker can rebuild any piece independently. This is
 * the property that makes the world streamable and infinite.
 */
export class World {
  constructor(configOverrides = {}) {
    this.config = makeConfig(configOverrides);
    this.seed = this.config.seed;
    this.chart = makeChart(this.config.world);
    this.fields = new MacroFields(this.config, this.chart);
    this.terrain = new Terrain(this.config, this.chart, this.fields);
    this.arterials = new ArterialGrid(this.config);
    this.cellNets = new LRU(64);
    this.roadViews = new LRU(32);
    this.cellPlans = new LRU(40);
    this.buildingPlans = new LRU(96);
    this.groundTiles = new LRU(768);
    /** Feature sources rasterized into chunks after the ground pass, in order. */
    this.featureSources = [];
    /** Optional world-level networks (set by installers). */
    this.highways = null;
    this.subway = null;
  }

  addFeatureSource(src) {
    this.featureSources.push(src);
    this.featureSources.sort((a, b) => a.order - b.order);
  }

  cellNet(i, j) {
    return this.cellNets.getOrCreate(`${i},${j}`, () => planCellNetwork(this, i, j));
  }

  cellPlan(i, j) {
    return this.cellPlans.getOrCreate(`${i},${j}`, () => planCell(this, i, j));
  }

  cellAt(x, y) {
    return this.arterials.cellAt(x, y);
  }

  /** Cells whose rect overlaps the query rect (inclusive voxel coords). */
  cellsOverlapping(rect) {
    const a = this.arterials.cellAt(rect.x0, rect.y0);
    const b = this.arterials.cellAt(rect.x1, rect.y1);
    const out = [];
    for (let j = a.j; j <= b.j; j += 1) for (let i = a.i; i <= b.i; i += 1) out.push({ i, j });
    return out;
  }

  /** Roads of cell (i,j) and all neighbours, with junction annotations. */
  roadView(i, j) {
    return this.roadViews.getOrCreate(`${i},${j}`, () => {
      const roads = [];
      for (let dj = -1; dj <= 1; dj += 1) {
        for (let di = -1; di <= 1; di += 1) {
          roads.push(...this.cellNet(i + di, j + dj).roads);
        }
      }
      return new RoadView(roads);
    });
  }

  /** Envelope by id ("C{i}_{j}/b../l../B"). */
  envelope(id) {
    const m = /^C(-?\d+)_(-?\d+)\//.exec(id);
    if (!m) return null;
    return this.cellPlan(Number(m[1]), Number(m[2])).buildingById.get(id) ?? null;
  }

  /** Building envelopes overlapping a world rect. */
  envelopesIn(rect) {
    const out = [];
    for (const { i, j } of this.cellsOverlapping(rect)) this.cellPlan(i, j).buildingsIn(rect, out);
    return out;
  }
}

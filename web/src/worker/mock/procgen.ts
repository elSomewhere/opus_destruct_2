/**
 * Procedural test worlds for the mock engine ('rooms', 'city', 'tower').
 *
 * Every world sits on an anchored bedrock layer (grid z = 0) under two layers of soil or
 * floor; the walkable ground surface is world z = 0. Structures are designed so that
 * shooting their supports detaches pieces (bridges, balconies, stacks, towers). Nothing
 * floats initially.
 */
import type { ProceduralKind, Vec3 } from '../../engine/protocol.ts';
import { Block } from './blocks.ts';
import { AIR, VoxelWorld } from './world.ts';

export interface GeneratedWorld {
  world: VoxelWorld;
  spawn: { pos: Vec3; dir: Vec3 };
  /** Light level 0..255 of an air cell (Doom sector light analogue). */
  light: (gx: number, gy: number, gz: number) => number;
}

/** Ground layers: bedrock (z=0), soil (z=1), surface (z=2). */
const GROUND = 3;

function mulberry32(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function hash2(x: number, y: number, seed: number): number {
  let h = Math.imul(x, 0x27d4eb2d) ^ Math.imul(y, 0x165667b1) ^ Math.imul(seed, 0x9e3779b9);
  h = Math.imul(h ^ (h >>> 15), 0x85ebca6b);
  return ((h ^ (h >>> 13)) >>> 0) / 4294967296;
}

/** Small building DSL over a world: boxes are half-open, in grid coordinates. */
class Builder {
  readonly world: VoxelWorld;
  constructor(world: VoxelWorld) {
    this.world = world;
  }
  box(x: number, y: number, z: number, sx: number, sy: number, sz: number, block: number): void {
    this.world.fillBox(x, y, z, x + sx, y + sy, z + sz, block);
  }
  clear(x: number, y: number, z: number, sx: number, sy: number, sz: number): void {
    this.box(x, y, z, sx, sy, sz, AIR);
  }
  ground(surface: (x: number, y: number) => number): void {
    const { nx, ny } = this.world;
    this.box(0, 0, 0, nx, ny, 1, Block.Bedrock);
    this.box(0, 0, 1, nx, ny, GROUND - 2, Block.Soil);
    for (let y = 0; y < ny; y++) for (let x = 0; x < nx; x++) this.world.set(x, y, GROUND - 1, surface(x, y));
  }
}

function makeWorld(sizeVoxels: Vec3, h: number, belowGroundLayers = GROUND): VoxelWorld {
  // World z = 0 is the top of the ground layers.
  return new VoxelWorld(sizeVoxels, h, [0, 0, -belowGroundLayers * h], Block.Bedrock);
}

// ---------------------------------------------------------------------------------------

function rooms(seed: number, h: number): GeneratedWorld {
  const m = (metres: number): number => Math.max(1, Math.round(metres / h));
  const R = m(16); // room size
  const W = m(0.5); // wall thickness
  const H = m(5); // wall height
  const N = 3;
  const span = N * R + (N + 1) * W;
  const world = makeWorld([span, span, GROUND + H + m(3)], h);
  const b = new Builder(world);
  const rand = mulberry32(seed);
  const z0 = GROUND;
  const roomX = (i: number): number => W + i * (R + W);

  const indoor = new Set(['0,0', '0,1', '0,2']);
  b.ground((x, y) => {
    const i = Math.min(N - 1, Math.max(0, Math.floor((x - W) / (R + W))));
    const j = Math.min(N - 1, Math.max(0, Math.floor((y - W) / (R + W))));
    return indoor.has(`${i},${j}`) ? Block.Slab : Block.Grass;
  });

  // Walls: a (N+1) x (N+1) grid of wall lines with doorways between rooms.
  for (let k = 0; k <= N; k++) {
    const p = k * (R + W);
    b.box(p, 0, z0, W, span, H, Block.Brick);
    b.box(0, p, z0, span, W, H, Block.Brick);
  }
  const door = m(2);
  const doorH = m(3);
  for (let k = 1; k < N; k++) {
    const p = k * (R + W);
    for (let r = 0; r < N; r++) {
      const c = roomX(r) + (R - door) / 2;
      b.clear(p, c, z0, W, door, doorH);
      b.clear(c, p, z0, door, W, doorH);
    }
  }

  const ceiling = (i: number, j: number, block: number): void => {
    b.box(roomX(i), roomX(j), z0 + H - m(0.375), R, R, m(0.375), block);
  };

  // (0,0) pillar hall under a ceiling.
  {
    const x = roomX(0);
    const y = roomX(0);
    for (let a = 1; a <= 3; a++) for (let c = 1; c <= 3; c++) b.box(x + (a * R) / 4 - 2, y + (c * R) / 4 - 2, z0, 4, 4, H, Block.Concrete);
    ceiling(0, 0, Block.Slab);
  }
  // (1,0) stairs up to a brick platform.
  {
    const x = roomX(1);
    const y = roomX(0);
    const steps = 8;
    for (let s = 0; s < steps; s++) b.box(x + m(2), y + m(1) + s * 4, z0, m(2), 4, (s + 1) * 2, Block.Concrete);
    b.box(x + m(2), y + m(1) + steps * 4, z0, m(8), m(6), steps * 2, Block.Brick);
  }
  // (2,0) bridge on two columns, clear of the walls: shoot both columns to drop it.
  {
    const x = roomX(2);
    const y = roomX(0);
    const zb = z0 + m(3);
    const len = R - 2 * m(1.5);
    const bx = x + m(1.5);
    const by = y + (R - m(2)) / 2;
    b.box(bx, by, zb, len, m(2), 2, Block.Slab);
    b.box(bx, by, zb + 2, len, 1, m(0.75), Block.Metal); // railings
    b.box(bx, by + m(2) - 1, zb + 2, len, 1, m(0.75), Block.Metal);
    for (const f of [1 / 3, 2 / 3]) b.box(bx + Math.round(len * f) - 2, by + m(1) - 2, z0, 4, 4, zb - z0, Block.Concrete);
  }
  // (0,1) low brick maze under a plaster ceiling.
  {
    const x = roomX(0);
    const y = roomX(1);
    for (let k = 0; k < 7; k++) {
      const horizontal = rand() < 0.5;
      const len = m(3 + rand() * 5);
      const px = x + Math.floor(rand() * (R - len));
      const py = y + Math.floor(rand() * (R - len));
      if (horizontal) b.box(px, py, z0, len, 2, m(2), Block.Brick);
      else b.box(px, py, z0, 2, len, m(2), Block.Brick);
    }
    ceiling(0, 1, Block.Plaster);
  }
  // (1,1) spawn room: steel frame carrying a platform (shoot all legs to drop it).
  {
    const x = roomX(1) + m(5);
    const y = roomX(1) + m(8);
    const s = m(5);
    const top = z0 + m(3.5);
    for (const [dx, dy] of [[0, 0], [s - 2, 0], [0, s - 2], [s - 2, s - 2]] as const) b.box(x + dx, y + dy, z0, 2, 2, top - z0, Block.Steel);
    b.box(x, y, top, s, 2, 2, Block.Steel);
    b.box(x, y + s - 2, top, s, 2, 2, Block.Steel);
    b.box(x, y, top, 2, s, 2, Block.Steel);
    b.box(x + s - 2, y, top, 2, s, 2, Block.Steel);
    b.box(x + 2, y + 2, top + 1, s - 4, s - 4, 1, Block.Hazard);
  }
  // (2,1) crate stacks: shoot a bottom crate to drop the ones above.
  {
    const x = roomX(2);
    const y = roomX(1);
    const c = m(1);
    for (let k = 0; k < 9; k++) {
      const px = x + m(2) + (k % 3) * m(4) + Math.floor(rand() * m(1));
      const py = y + m(2) + Math.floor(k / 3) * m(4) + Math.floor(rand() * m(1));
      const height = 1 + Math.floor(rand() * 3);
      for (let s = 0; s < height; s++) b.box(px, py, z0 + s * c, c, c, c, s % 2 === 0 ? Block.Concrete : Block.Metal);
    }
  }
  // (0,2) covered hall: an RC slab on columns with a mezzanine opening.
  {
    const x = roomX(0);
    const y = roomX(2);
    const zs = z0 + m(2.75);
    for (let a = 0; a < 3; a++) for (let c = 0; c < 3; c++) b.box(x + m(2) + a * m(5.5), y + m(2) + c * m(5.5), z0, 4, 4, zs - z0, Block.Concrete);
    b.box(x + m(1), y + m(1), zs, R - m(2), R - m(2), 2, Block.Slab);
    b.clear(x + m(6), y + m(6), zs, m(4), m(4), 2);
  }
  // (1,2) colonnade: a brick wall with arches.
  {
    const x = roomX(1);
    const y = roomX(2) + R / 2 - 2;
    const wallH = m(4);
    b.box(x + m(1), y, z0, R - m(2), 4, wallH, Block.Brick);
    const archW = m(2);
    const archH = m(2.5);
    for (let a = 0; a < 5; a++) {
      const ax = x + m(1.75) + a * m(2.75);
      for (let dz = 0; dz < archH + archW / 2; dz++) {
        for (let dx = 0; dx < archW; dx++) {
          const cx = dx + 0.5 - archW / 2;
          const cz = dz + 0.5 - archH;
          if (dz < archH || cx * cx + cz * cz < (archW / 2) * (archW / 2)) b.clear(ax + dx, y, z0 + dz, 1, 4, 1);
        }
      }
    }
  }
  // (2,2) cantilever balcony from the wall, propped by a steel post.
  {
    const x = roomX(2) + m(5);
    const y = roomX(2) + R - m(4);
    const zb = z0 + m(3);
    b.box(x, y, zb, m(6), m(4), 3, Block.Slab);
    b.box(x, y, zb + 3, m(6), 1, m(1), Block.Metal);
    b.box(x + m(3) - 1, y + 1, z0, 2, 2, zb - z0, Block.Steel);
  }

  const lightOfRoom = [
    [150, 176, 200],
    [255, 255, 224],
    [255, 255, 240],
  ];
  const light = (gx: number, gy: number, gz: number): number => {
    if (gz >= world.columnTop(gx, gy)) return 255;
    const i = Math.min(N - 1, Math.max(0, Math.floor((gx - W) / (R + W))));
    const j = Math.min(N - 1, Math.max(0, Math.floor((gy - W) / (R + W))));
    const l = lightOfRoom[i]?.[j] ?? 200;
    return l === 255 ? 208 : l; // shaded spots under structures in open rooms
  };

  return {
    world,
    spawn: { pos: [(roomX(1) + R / 2) * h, (roomX(1) + m(2)) * h, 0], dir: [0, 1, 0] },
    light,
  };
}

// ---------------------------------------------------------------------------------------

function city(seed: number, h: number): GeneratedWorld {
  const m = (metres: number): number => Math.max(1, Math.round(metres / h));
  const P = m(16); // plot
  const S = m(4); // street
  const N = 3;
  const span = S + N * (P + S);
  const storeyH = m(3);
  const maxStoreys = 6;
  const world = makeWorld([span, span, GROUND + maxStoreys * storeyH + 8], h);
  const b = new Builder(world);
  const z0 = GROUND;
  const plotX = (i: number): number => S + i * (P + S);
  const inPlot = (v: number): boolean => {
    const r = (v - S) % (P + S);
    return v >= S && r >= 0 && r < P;
  };
  b.ground((x, y) => (inPlot(x) && inPlot(y) ? Block.Grass : Block.Concrete));

  const lights: number[] = [];
  for (let j = 0; j < N; j++) {
    for (let i = 0; i < N; i++) {
      const r = hash2(i, j, seed);
      lights.push(150 + Math.floor(hash2(j, i, seed + 1) * 70));
      if (i === 1 && j === 1) continue; // central square stays open
      const F = m(10);
      const x = plotX(i) + (P - F) / 2;
      const y = plotX(j) + (P - F) / 2;
      const storeys = 2 + Math.floor(r * (maxStoreys - 1));
      const steel = r > 0.8;
      const colPos = [0, 25, 51, F - 4];
      for (let s = 0; s < storeys; s++) {
        const zf = z0 + s * storeyH;
        // Floor slab (the ground floor sits on the soil).
        b.box(x, y, zf, F, F, 2, steel ? Block.Metal : Block.Slab);
        for (const cx of colPos) for (const cy of colPos) b.box(x + cx, y + cy, zf + 2, 4, 4, storeyH - 2, steel ? Block.Steel : Block.Concrete);
        if (!steel) {
          const wallH = storeyH - 2;
          for (const [wx, wy, lx, ly] of [
            [x, y, F, 2],
            [x, y + F - 2, F, 2],
            [x, y, 2, F],
            [x + F - 2, y, 2, F],
          ] as const) {
            b.box(wx, wy, zf + 2, lx, ly, wallH, Block.Brick);
            // Windows every 20 voxels (sill 1 m, 1.5 x 1.5 m).
            for (let k = 8; k + 12 < Math.max(lx, ly); k += 20) {
              const ox = lx > ly ? k : 0;
              const oy = lx > ly ? 0 : k;
              b.clear(wx + ox, wy + oy, zf + 2 + m(1), lx > ly ? 12 : 2, lx > ly ? 2 : 12, 12);
            }
          }
          if (s === 0) b.clear(x + F / 2 - 8, y, zf + 2, 16, 2, m(2.5)); // door
        }
      }
      b.box(x, y, z0 + storeys * storeyH, F, F, 2, steel ? Block.Metal : Block.Slab); // roof
    }
  }
  // Central square: a monument on a thin stem (shoot the stem).
  {
    const cx = plotX(1) + P / 2;
    const cy = plotX(1) + P / 2;
    b.box(cx - m(1.5), cy - m(1.5), z0, m(3), m(3), m(0.5), Block.Concrete);
    b.box(cx - 2, cy - 2, z0 + m(0.5), 4, 4, m(3), Block.Steel);
    b.box(cx - m(1), cy - m(1), z0 + m(3.5), m(2), m(2), m(2), Block.Brick);
  }

  const light = (gx: number, gy: number, gz: number): number => {
    if (gz >= world.columnTop(gx, gy)) return 255;
    const i = Math.min(N - 1, Math.max(0, Math.floor((gx - S) / (P + S))));
    const j = Math.min(N - 1, Math.max(0, Math.floor((gy - S) / (P + S))));
    return lights[i + N * j] ?? 180;
  };
  const street = S / 2 + (P + S);
  return { world, spawn: { pos: [street * h, (S / 2) * h, 0], dir: [0, 1, 0] }, light };
}

// ---------------------------------------------------------------------------------------

function tower(seed: number, h: number): GeneratedWorld {
  const m = (metres: number): number => Math.max(1, Math.round(metres / h));
  const span = m(40);
  const storeys = 12;
  const storeyH = m(3);
  const world = makeWorld([span, span, GROUND + storeys * storeyH + 8], h);
  const b = new Builder(world);
  const rand = mulberry32(seed);
  const z0 = GROUND;
  b.ground(() => Block.Grass);
  const F = m(8);
  const x = (span - F) / 2;
  const y = (span - F) / 2;
  for (let s = 0; s < storeys; s++) {
    const zf = z0 + s * storeyH;
    b.box(x, y, zf, F, F, 2, Block.Slab);
    const colH = storeyH - 2;
    // Corner columns 0.75 m, mid-edge columns 0.5 m.
    for (const cx of [0, F - 6]) for (const cy of [0, F - 6]) b.box(x + cx, y + cy, zf + 2, 6, 6, colH, Block.Concrete);
    for (const [cx, cy] of [[F / 2 - 2, 0], [F / 2 - 2, F - 4], [0, F / 2 - 2], [F - 4, F / 2 - 2]] as const) b.box(x + cx, y + cy, zf + 2, 4, 4, colH, Block.Concrete);
    if (s % 2 === 1) {
      // Brick infill with windows on odd storeys.
      for (const [wx, wy, lx, ly] of [
        [x + 6, y, F - 12, 2],
        [x + 6, y + F - 2, F - 12, 2],
        [x, y + 6, 2, F - 12],
        [x + F - 2, y + 6, 2, F - 12],
      ] as const) {
        b.box(wx, wy, zf + 2, lx, ly, colH, Block.Brick);
        const len = Math.max(lx, ly);
        for (let k = 4; k + 10 <= len; k += 14) {
          b.clear(wx + (lx > ly ? k : 0), wy + (lx > ly ? 0 : k), zf + 2 + m(1), lx > ly ? 10 : 2, lx > ly ? 2 : 10, 10);
        }
      }
    }
    if (s > 0 && rand() < 0.5) {
      // Cantilever balcony on the south face.
      b.box(x + F / 2 - m(1.5), y - m(1.5), zf, m(3), m(1.5), 2, Block.Slab);
      b.box(x + F / 2 - m(1.5), y - m(1.5), zf + 2, m(3), 1, m(1), Block.Metal);
    }
  }
  b.box(x, y, z0 + storeys * storeyH, F, F, 2, Block.Slab);

  const light = (gx: number, gy: number, gz: number): number => (gz >= world.columnTop(gx, gy) ? 255 : 176);
  return {
    world,
    spawn: { pos: [(span / 2) * h, (y - m(14)) * h, 0], dir: [0, 1, 0.12] },
    light,
  };
}

export function generateWorld(kind: ProceduralKind, seed: number, voxelSize: number): GeneratedWorld {
  switch (kind) {
    case 'rooms':
      return rooms(seed, voxelSize);
    case 'city':
    case 'drive': // (the mock has no roads or vehicles: its city)
      return city(seed, voxelSize);
    case 'tower':
      return tower(seed, voxelSize);
    case 'yard':
    case 'angles':
    case 'machines':
      return rooms(seed, voxelSize); // (the mock has none of them: the real engine builds them)
  }
}

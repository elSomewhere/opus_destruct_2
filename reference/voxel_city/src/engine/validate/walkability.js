import { buildChunk, groundTile } from "../voxel/compose.js";
import { IS_SOLID, IS_CLIMB } from "../voxel/materials.js";
import { P, P2 } from "../voxel/chunk.js";
import { CHUNK } from "../core/units.js";
import { frameOf } from "../buildings/frame.js";
import { ROOM0 } from "../buildings/interior/grid.js";

/**
 * Voxel-level walkability check for a building: flood-fills standable
 * positions of a walker with the viewer's real footprint (4x4 voxels =
 * 0.5 m, 14 voxels = 1.75 m headroom, steps of at most 2 voxels) starting on
 * the street in front of the building, through the actual generated voxels.
 * Reports every room that cannot be stood in.
 */

const FOOT = 4;
const HEAD = 14;
const STEP = 2;
const EXEMPT = new Set(["shaft", "elevator", "mechanicalShaft", "void"]);

export function walkabilityReport(world, env, { maxFloors = 8, margin = 24, debug = false } = {}) {
  const plan = world.buildingPlan(env);
  if (!plan) return { ok: true, skipped: true, missing: [] };
  const floors = plan.floors.filter((f) => f.index < maxFloors);
  const zTop = Math.max(...floors.map((f) => f.z + f.height)) + 4;
  const zBot = Math.min(env.groundZ - 2, ...floors.map((f) => f.z)) - 2;
  const R = env.bounds;
  const x0 = R.x0 - margin;
  const y0 = R.y0 - margin;
  const X = R.x1 + margin - x0 + 1;
  const Y = R.y1 + margin - y0 + 1;
  const Z = zTop - zBot + 1;
  // dense solidity volume from real chunks
  const solid = new Uint8Array(X * Y * Z);
  const idx = (x, y, z) => x + y * X + z * X * Y;
  const cx0 = Math.floor(x0 / CHUNK);
  const cy0 = Math.floor(y0 / CHUNK);
  const cz0 = Math.floor(zBot / CHUNK);
  const cx1 = Math.floor((x0 + X - 1) / CHUNK);
  const cy1 = Math.floor((y0 + Y - 1) / CHUNK);
  const cz1 = Math.floor(zTop / CHUNK);
  for (let cy = cy0; cy <= cy1; cy += 1) {
    for (let cx = cx0; cx <= cx1; cx += 1) {
      const tile = groundTile(world, 0, cx, cy);
      for (let cz = cz0; cz <= cz1; cz += 1) {
        const ch = buildChunk(world, 0, cx, cy, cz, tile);
        for (let k = 1; k <= CHUNK; k += 1) {
          const z = cz * CHUNK + k - 1 - zBot;
          if (z < 0 || z >= Z) continue;
          for (let j = 1; j <= CHUNK; j += 1) {
            const y = cy * CHUNK + j - 1 - y0;
            if (y < 0 || y >= Y) continue;
            for (let i = 1; i <= CHUNK; i += 1) {
              const x = cx * CHUNK + i - 1 - x0;
              if (x < 0 || x >= X) continue;
              if (IS_SOLID[ch.data[i + j * P + k * P2]]) solid[idx(x, y, z)] = 1;
            }
          }
        }
      }
    }
  }
  // headroom: free[x,y,z] = HEAD consecutive non-solid voxels from z upward
  const up = new Uint8Array(X * Y * Z);
  for (let y = 0; y < Y; y += 1) {
    for (let x = 0; x < X; x += 1) {
      let run = 0;
      for (let z = Z - 1; z >= 0; z -= 1) {
        const k = idx(x, y, z);
        run = solid[k] ? 0 : Math.min(255, run + 1);
        up[k] = run;
      }
    }
  }
  const fits = (x, y, z) => {
    if (x < 0 || y < 0 || x + FOOT > X || y + FOOT > Y || z < 1 || z + HEAD >= Z) return false;
    let support = false;
    for (let dy = 0; dy < FOOT; dy += 1) {
      for (let dx = 0; dx < FOOT; dx += 1) {
        const k = idx(x + dx, y + dy, z);
        if (up[k] < HEAD) return false;
        if (solid[k - X * Y]) support = true;
      }
    }
    return support;
  };
  // settle: drop until supported (max 40)
  const settle = (x, y, z) => {
    for (let d = 0; d < 40 && z > 1; d += 1) {
      if (fits(x, y, z)) return z;
      let blocked = false;
      for (let dy = 0; dy < FOOT && !blocked; dy += 1) for (let dx = 0; dx < FOOT; dx += 1) if (up[idx(x + dx, y + dy, z)] < HEAD) blocked = true;
      if (blocked) return -1;
      z -= 1;
    }
    return -1;
  };
  // start: in front of the entrance on the street side
  const frame = frameOf(env);
  const g0 = plan.floorByIndex.get(0);
  const entrance = g0.grid.doors.find((d) => d.b === -1 && d.kind !== "balcony") ?? null;
  const visited = new Uint8Array(X * Y * Z);
  const queue = [];
  const seeds = [];
  if (entrance) {
    const mu = (entrance.u0 + entrance.u1) / 2;
    const mv = (entrance.v0 + entrance.v1) / 2;
    for (const [du, dv] of [
      [0, -6],
      [0, 6],
      [-6, 0],
      [6, 0],
    ]) {
      const [wx, wy] = frame.toWorld(Math.round(mu + du), Math.round(mv + dv));
      if (g0.grid.get(Math.round(mu + du), Math.round(mv + dv)) === 0) seeds.push([wx, wy]);
    }
  }
  for (const [wx, wy] of seeds) {
    const x = wx - x0 - 2;
    const y = wy - y0 - 2;
    // lowest standable spot at street level (not on top of canopies)
    for (let z = Math.max(1, env.groundZ - 6 - zBot); z < Z - HEAD - 1; z += 1) {
      if (fits(x, y, z)) {
        visited[idx(x, y, z)] = 1;
        queue.push(x, y, z);
        break;
      }
    }
  }
  let head = 0;
  while (head < queue.length) {
    const x = queue[head++];
    const y = queue[head++];
    const z = queue[head++];
    for (const [dx, dy] of [
      [1, 0],
      [-1, 0],
      [0, 1],
      [0, -1],
    ]) {
      const nx = x + dx;
      const ny = y + dy;
      for (let dz = STEP; dz >= -STEP; dz -= 1) {
        const nz = z + dz;
        if (nz < 1 || nz + HEAD >= Z) continue;
        if (!fits(nx, ny, nz)) continue;
        // stepping up needs headroom at the current column too
        const k = idx(nx, ny, nz);
        if (!visited[k]) {
          visited[k] = 1;
          queue.push(nx, ny, nz);
        }
        break;
      }
      // walking off a ledge: fall
      if (fits(nx, ny, z - STEP - 1) === false) {
        const fz = settle(nx, ny, z);
        if (fz > 0) {
          const k = idx(nx, ny, fz);
          if (!visited[k]) {
            visited[k] = 1;
            queue.push(nx, ny, fz);
          }
        }
      }
    }
  }
  // room coverage
  const missing = [];
  const checked = new Set();
  for (const F of floors) {
    const grid = F.grid;
    for (const room of grid.rooms) {
      if (EXEMPT.has(room.type)) continue;
      const key = `${F.index}:${room.id}`;
      if (checked.has(key)) continue;
      checked.add(key);
      let ok = false;
      const zLo = F.z + 2 - zBot - STEP;
      const zHi = room.type === "stair" ? F.z + F.height - zBot : F.z + 2 - zBot + STEP;
      for (const r of room.rects) {
        for (let v = r.y0; v <= r.y1 && !ok; v += 1) {
          for (let u = r.x0; u <= r.x1 && !ok; u += 1) {
            if (grid.get(u, v) !== ROOM0 + room.id) continue;
            const [wx, wy] = frame.toWorld(u, v);
            const x = wx - x0;
            const y = wy - y0;
            // any visited footprint covering this cell
            for (let z = Math.max(1, zLo); z <= Math.min(Z - 1, zHi) && !ok; z += 1) {
              for (let dy = -FOOT + 1; dy <= 0 && !ok; dy += 1)
                for (let dx = -FOOT + 1; dx <= 0 && !ok; dx += 1) {
                  const xx = x + dx;
                  const yy = y + dy;
                  if (xx < 0 || yy < 0 || xx >= X || yy >= Y) continue;
                  if (visited[idx(xx, yy, z)]) ok = true;
                }
            }
          }
        }
      }
      if (!ok) missing.push({ floor: F.index, room: room.id, type: room.type, area: grid.area(room) });
    }
  }
  const res = { ok: missing.length === 0, missing, reached: queue.length / 3, seeds: seeds.length };
  if (debug) Object.assign(res, { solid, visited, up, fits, X, Y, Z, x0, y0, zBot, frame });
  return res;
}

/**
 * Generic walker flood over an arbitrary world region (LOD0 voxels).
 * region: {x0,y0,z0,x1,y1,z1}; seeds: [[x,y,z]] world positions (feet).
 * With `climb`, the walker also climbs ladders like the viewer does: any
 * position within reach of a climbable voxel may move one voxel up or down
 * without support.
 * Returns { reached(x,y,z,tol) } to query reachability of targets.
 */
export function floodRegion(world, region, seeds, { climb = false } = {}) {
  const x0 = region.x0;
  const y0 = region.y0;
  const zBot = region.z0;
  const X = region.x1 - region.x0 + 1;
  const Y = region.y1 - region.y0 + 1;
  const Z = region.z1 - region.z0 + 1;
  const solid = new Uint8Array(X * Y * Z);
  const ladder = climb ? new Uint8Array(X * Y * Z) : null;
  const idx = (x, y, z) => x + y * X + z * X * Y;
  for (let cy = Math.floor(y0 / CHUNK); cy <= Math.floor((y0 + Y - 1) / CHUNK); cy += 1) {
    for (let cx = Math.floor(x0 / CHUNK); cx <= Math.floor((x0 + X - 1) / CHUNK); cx += 1) {
      const tile = groundTile(world, 0, cx, cy);
      for (let cz = Math.floor(zBot / CHUNK); cz <= Math.floor((zBot + Z - 1) / CHUNK); cz += 1) {
        const ch = buildChunk(world, 0, cx, cy, cz, tile);
        for (let k = 1; k <= CHUNK; k += 1) {
          const z = cz * CHUNK + k - 1 - zBot;
          if (z < 0 || z >= Z) continue;
          for (let j = 1; j <= CHUNK; j += 1) {
            const y = cy * CHUNK + j - 1 - y0;
            if (y < 0 || y >= Y) continue;
            for (let i = 1; i <= CHUNK; i += 1) {
              const x = cx * CHUNK + i - 1 - x0;
              if (x < 0 || x >= X) continue;
              const m = ch.data[i + j * P + k * P2];
              if (IS_SOLID[m]) solid[idx(x, y, z)] = 1;
              if (ladder && IS_CLIMB[m]) ladder[idx(x, y, z)] = 1;
            }
          }
        }
      }
    }
  }
  const up = new Uint8Array(X * Y * Z);
  for (let y = 0; y < Y; y += 1)
    for (let x = 0; x < X; x += 1) {
      let run = 0;
      for (let z = Z - 1; z >= 0; z -= 1) {
        const k = idx(x, y, z);
        run = solid[k] ? 0 : Math.min(255, run + 1);
        up[k] = run;
      }
    }
  const free = (x, y, z) => {
    if (x < 0 || y < 0 || x + FOOT > X || y + FOOT > Y || z < 1 || z + HEAD >= Z) return false;
    for (let dy = 0; dy < FOOT; dy += 1) for (let dx = 0; dx < FOOT; dx += 1) if (up[idx(x + dx, y + dy, z)] < HEAD) return false;
    return true;
  };
  const fits = (x, y, z) => {
    if (!free(x, y, z)) return false;
    for (let dy = 0; dy < FOOT; dy += 1) for (let dx = 0; dx < FOOT; dx += 1) if (solid[idx(x + dx, y + dy, z - 1)]) return true;
    return false;
  };
  const touching = (x, y, z) => {
    if (!ladder) return false;
    for (let zz = Math.max(0, z - 2); zz <= Math.min(Z - 1, z + 8); zz += 1)
      for (let yy = Math.max(0, y - 1); yy <= Math.min(Y - 1, y + FOOT); yy += 1)
        for (let xx = Math.max(0, x - 1); xx <= Math.min(X - 1, x + FOOT); xx += 1) if (ladder[idx(xx, yy, zz)]) return true;
    return false;
  };
  const valid = (x, y, z) => fits(x, y, z) || (ladder !== null && free(x, y, z) && touching(x, y, z));
  const visited = new Uint8Array(X * Y * Z);
  const queue = [];
  const push = (x, y, z) => {
    const k = idx(x, y, z);
    if (visited[k]) return;
    visited[k] = 1;
    queue.push(x, y, z);
  };
  for (const [sx, sy, sz] of seeds) {
    const x = sx - x0 - 2;
    const y = sy - y0 - 2;
    for (let z = sz - zBot - 3; z <= sz - zBot + 6; z += 1) {
      if (fits(x, y, z)) {
        push(x, y, z);
        break;
      }
    }
  }
  let head = 0;
  while (head < queue.length) {
    const x = queue[head++];
    const y = queue[head++];
    const z = queue[head++];
    for (const [dx, dy] of [
      [1, 0],
      [-1, 0],
      [0, 1],
      [0, -1],
    ]) {
      for (let dz = STEP; dz >= -8; dz -= 1) {
        const nz = z + dz;
        if (!valid(x + dx, y + dy, nz)) continue;
        push(x + dx, y + dy, nz);
        break;
      }
    }
    if (ladder && touching(x, y, z)) {
      if (valid(x, y, z + 1)) push(x, y, z + 1);
      if (valid(x, y, z - 1)) push(x, y, z - 1);
    }
  }
  return {
    count: queue.length / 3,
    /** Any standable position reached inside a world rect at walking height wz (± tol)? */
    reachedRect(r, wz, tol = 3) {
      for (let z = wz - zBot - tol; z <= wz - zBot + tol; z += 1) {
        if (z < 0 || z >= Z) continue;
        for (let y = Math.max(0, r.y0 - y0 - 3); y <= Math.min(Y - 1, r.y1 - y0); y += 1)
          for (let x = Math.max(0, r.x0 - x0 - 3); x <= Math.min(X - 1, r.x1 - x0); x += 1) if (visited[idx(x, y, z)]) return true;
      }
      return false;
    },
    reached(wx, wy, wz, tol = 4) {
      const cx = wx - x0;
      const cy = wy - y0;
      for (let z = wz - zBot - tol; z <= wz - zBot + tol; z += 1)
        for (let dy = -6; dy <= 2; dy += 1)
          for (let dx = -6; dx <= 2; dx += 1) {
            const x = cx + dx;
            const y = cy + dy;
            if (x < 0 || y < 0 || x >= X || y >= Y || z < 0 || z >= Z) continue;
            if (visited[idx(x, y, z)]) return true;
          }
      return false;
    },
  };
}

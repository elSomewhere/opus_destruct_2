/**
 * Unit and end-to-end tests of the mock engine (the protocol's reference implementation).
 * Run with `npm test` (node --test, native TypeScript type stripping).
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import type { ChunkMesh, DetachedEvent, EngineEvent, MeshData, Vec3, WorkerMessage } from '../src/engine/protocol.ts';
import { DEBRIS_STRIDE, DebugView, DEFAULT_PARAMS, TEXTURE_MATERIAL_BASE, VERTEX_STRIDE } from '../src/engine/protocol.ts';
import { decodeVertex, MeshBuilder, vertexBounds } from '../src/engine/vertex.ts';
import { Block, resolveFaceTextures } from '../src/worker/mock/blocks.ts';
import { collideAabb } from '../src/worker/mock/collide.ts';
import { moveWithStep, type CollideFn } from '../src/game/stepmove.ts';
import { findIslands } from '../src/worker/mock/connectivity.ts';
import { MockEngine } from '../src/worker/mock/engine.ts';
import { fillPaddedChunk, meshBlock, PADDED_CHUNK_VOLUME, type MeshStyle } from '../src/worker/mock/mesher.ts';
import { raycast } from '../src/worker/mock/raycast.ts';
import { AIR, VoxelWorld } from '../src/worker/mock/world.ts';

const H = 0.125;

function style(greedy = true): MeshStyle {
  return {
    faceTextures: resolveFaceTextures(new Map()),
    texelsPerMetre: 32,
    light: () => 200,
    debug: null,
    greedy,
  };
}

/** Meshes a small dense block (blocks[x + sx*(y + sy*z)]) at the world origin. */
function meshDense(sx: number, sy: number, sz: number, fill: (x: number, y: number, z: number) => number, greedy = true): MeshData {
  const px = sx + 2;
  const py = sy + 2;
  const blocks = new Uint8Array(px * py * (sz + 2));
  for (let z = 0; z < sz; z++)
    for (let y = 0; y < sy; y++) for (let x = 0; x < sx; x++) blocks[x + 1 + px * (y + 1 + py * (z + 1))] = fill(x, y, z);
  const b = new MeshBuilder(16);
  meshBlock({ blocks, sx, sy, sz, origin: [0, 0, 0], grid: [0, 0, 0], h: H }, style(greedy), b);
  return b.finish();
}

/** Every triangle must be CCW seen from outside: its geometric normal agrees with the vertex normal. */
function assertWindingAndIndices(m: MeshData): void {
  assert.equal(m.vertices.byteLength, m.vertexCount * VERTEX_STRIDE);
  assert.equal(m.indices.byteLength, m.indexCount * 4);
  assert.equal(m.indexCount % 3, 0);
  const idx = new Uint32Array(m.indices);
  for (let t = 0; t < m.indexCount; t += 3) {
    const [a, b, c] = [idx[t]!, idx[t + 1]!, idx[t + 2]!];
    for (const i of [a, b, c]) assert.ok(i < m.vertexCount, 'index in range');
    const va = decodeVertex(m.vertices, a);
    const vb = decodeVertex(m.vertices, b);
    const vc = decodeVertex(m.vertices, c);
    const e1 = [vb.pos[0] - va.pos[0], vb.pos[1] - va.pos[1], vb.pos[2] - va.pos[2]];
    const e2 = [vc.pos[0] - va.pos[0], vc.pos[1] - va.pos[1], vc.pos[2] - va.pos[2]];
    const n = [e1[1]! * e2[2]! - e1[2]! * e2[1]!, e1[2]! * e2[0]! - e1[0]! * e2[2]!, e1[0]! * e2[1]! - e1[1]! * e2[0]!];
    const dot = n[0]! * va.normal[0] + n[1]! * va.normal[1] + n[2]! * va.normal[2];
    assert.ok(dot > 0, `triangle ${t / 3} is CCW from outside`);
  }
}

test('single voxel: 6 quads, CCW, open AO, material texture id', () => {
  const m = meshDense(1, 1, 1, () => Block.Steel);
  assert.equal(m.vertexCount, 24);
  assert.equal(m.indexCount, 36);
  assertWindingAndIndices(m);
  for (let i = 0; i < m.vertexCount; i++) {
    const v = decodeVertex(m.vertices, i);
    assert.equal(v.ao, 1);
    assert.equal(v.light, 200);
    assert.equal(v.texture, TEXTURE_MATERIAL_BASE + 2, 'steel is untextured -> material colour');
    assert.ok(Math.abs(Math.hypot(...v.normal) - 1) < 1e-6);
  }
  const b = vertexBounds(m.vertices, m.vertexCount)!;
  assert.deepEqual(b, { min: [0, 0, 0], max: [H, H, H] });
});

test('greedy merging: a 4x4x1 slab is 6 quads; culled mode is 4*4*2 + 16 quads', () => {
  const g = meshDense(4, 4, 1, () => Block.Concrete);
  assert.equal(g.indexCount / 6, 6);
  assertWindingAndIndices(g);
  const c = meshDense(4, 4, 1, () => Block.Concrete, false);
  assert.equal(c.indexCount / 6, 16 * 2 + 16);
  assertWindingAndIndices(c);
});

test('ambient occlusion darkens the inner corner of an L', () => {
  // Floor 3x3 plus one voxel standing on (1,1): floor top faces touching it get AO < 1.
  const m = meshDense(3, 3, 2, (x, y, z) => (z === 0 || (x === 1 && y === 1) ? Block.Concrete : AIR), false);
  assertWindingAndIndices(m);
  let occluded = 0;
  for (let i = 0; i < m.vertexCount; i++) {
    const v = decodeVertex(m.vertices, i);
    if (v.normal[2] > 0.9 && Math.abs(v.pos[2] - H) < 1e-6 && v.ao < 1) occluded++;
  }
  assert.ok(occluded >= 8, `found ${occluded} occluded floor vertices`);
});

function flatWorld(): VoxelWorld {
  // 64 x 64 x 64 voxels, ground layers z = 0..2, ground surface at world z = 0.
  const w = new VoxelWorld([64, 64, 64], H, [0, 0, -3 * H], Block.Bedrock);
  w.fillBox(0, 0, 0, 64, 64, 1, Block.Bedrock);
  w.fillBox(0, 0, 1, 64, 64, 3, Block.Soil);
  return w;
}

test('world bookkeeping: counts, column tops, chunk release', () => {
  const w = flatWorld();
  assert.equal(w.solidCount, 64 * 64 * 3);
  assert.equal(w.columnTop(5, 5), 3);
  w.set(5, 5, 10, Block.Brick);
  assert.equal(w.columnTop(5, 5), 11);
  w.set(5, 5, 10, AIR);
  assert.equal(w.columnTop(5, 5), 3);
  assert.equal(w.get(5, 5, -1), Block.Bedrock, 'below the grid is bedrock');
  assert.equal(w.get(-1, 5, 1), AIR, 'outside laterally is air');
  const ci = w.chunkIndex(0, 0, 1);
  w.set(3, 3, 40, Block.Brick);
  assert.ok(w.chunkData(ci));
  w.set(3, 3, 40, AIR);
  assert.equal(w.chunkData(ci), null, 'empty chunk is released');
});

test('raycast: hits the ground from above and a wall from the side', () => {
  const w = flatWorld();
  const down = raycast(w, [2, 2, 3], [0, 0, -1], 100);
  assert.ok(down);
  assert.ok(Math.abs(down.pos[2]) < 1e-6);
  assert.ok(Math.abs(down.distance - 3) < 1e-6);
  assert.deepEqual(down.normal, [0, 0, 1]);
  w.fillBox(40, 0, 3, 42, 64, 20, Block.Brick); // wall at x = 5 m
  const side = raycast(w, [1, 4, 1], [1, 0, 0], 100);
  assert.ok(side);
  assert.ok(Math.abs(side.pos[0] - 5) < 1e-6);
  assert.deepEqual(side.normal, [-1, 0, 0]);
  assert.equal(side.material, 3 /* masonry */);
  assert.equal(raycast(w, [1, 4, 1], [0, 0, 1], 100), null, 'sky');
  const outside = raycast(w, [-5, 4, 1], [1, 0, 0], 100);
  assert.ok(outside && Math.abs(outside.pos[0] - 5) < 1e-6, 'ray entering the grid from outside');
});

test('collide: falls onto the ground and slides along walls', () => {
  const w = flatWorld();
  const min: Vec3 = [2, 2, 0.5];
  const max: Vec3 = [2.6, 2.6, 2.25];
  const fall = collideAabb(w, min, max, [0, 0, -2]);
  assert.ok(Math.abs(fall.move[2] + 0.5) < 1e-3, `fell ${fall.move[2]}`);
  assert.ok(fall.onGround);
  const stand: Vec3 = [2, 2, fall.move[2] + 0.5];
  const standMax: Vec3 = [2.6, 2.6, stand[2] + 1.75];
  w.fillBox(24, 0, 3, 26, 64, 20, Block.Brick); // wall at x = 3 m
  const blocked = collideAabb(w, stand, standMax, [1, 0.5, 0]);
  assert.ok(Math.abs(blocked.move[0] - 0.4) < 1e-3, `slid to the wall: ${blocked.move[0]}`);
  assert.ok(Math.abs(blocked.move[1] - 0.5) < 1e-6, 'slides along the wall');
  assert.ok(blocked.onGround);
  const jump = collideAabb(w, stand, standMax, [0, 0, 0.3]);
  assert.ok(Math.abs(jump.move[2] - 0.3) < 1e-6 && !jump.onGround, 'free vertical move');
});

test('client step-up (standard collide sweeps only): climbs a ledge, not a wall', async () => {
  const w = flatWorld();
  const sweep: CollideFn = (a, b, m) => Promise.resolve(collideAabb(w, a, b, m));
  const min: Vec3 = [2, 2, 0.0001];
  const max: Vec3 = [2.6, 2.6, 1.7501];
  w.fillBox(0, 22, 3, 20, 64, 4, Block.Concrete); // one-voxel ledge from y = 2.75 m
  const noStep = await moveWithStep(sweep, min, max, [0, 0.5, -0.01], 0, true);
  assert.ok(noStep.move[1] < 0.2 && noStep.stepped === 0);
  const step = await moveWithStep(sweep, min, max, [0, 0.5, -0.01], 0.3, true);
  assert.ok(Math.abs(step.move[1] - 0.5) < 1e-6, `stepped forward ${step.move[1]}`);
  assert.ok(Math.abs(step.move[2] - 0.125) < 2e-3, `onto the ledge: ${step.move[2]}`);
  assert.ok(step.onGround && step.stepped > 0.1);
  w.fillBox(0, 30, 4, 20, 64, 16, Block.Brick); // 1.5 m wall from y = 3.75 m
  const onLedge: Vec3 = [2, 3.0, 0.1251];
  const wall = await moveWithStep(sweep, onLedge, [2.6, 3.6, 1.8751], [0, 0.5, -0.01], 0.3, true);
  assert.equal(wall.stepped, 0, 'a wall is not a step');
  assert.ok(Math.abs(wall.move[1] - 0.15) < 2e-3, `stopped at the wall: ${wall.move[1]}`);
  const airborne = await moveWithStep(sweep, min, max, [0, 0.5, 0.05], 0.3, false);
  assert.equal(airborne.stepped, 0, 'no stepping in the air');
});

test('connectivity: cutting a pillar detaches the part above', () => {
  const w = flatWorld();
  w.fillBox(10, 10, 3, 12, 12, 20, Block.Concrete); // pillar
  w.fillBox(6, 6, 20, 16, 16, 22, Block.Slab); // slab on top
  for (let z = 10; z < 12; z++) for (let y = 10; y < 12; y++) for (let x = 10; x < 12; x++) w.set(x, y, z, AIR);
  const seeds = [w.pack(10, 10, 12), w.pack(10, 10, 9)];
  const { islands } = findIslands(w, seeds, 1e6);
  assert.equal(islands.length, 1);
  assert.equal(islands[0]!.voxels.length, 2 * 2 * 8 + 10 * 10 * 2);
  const again = findIslands(w, [w.pack(10, 10, 9)], 1e6);
  assert.equal(again.islands.length, 0, 'the stump stays supported');
});

function runEngine(): { engine: MockEngine; messages: WorkerMessage[]; tick: (ms: number) => void; now: () => number } {
  const messages: WorkerMessage[] = [];
  const engine = new MockEngine((m) => messages.push(m));
  let t = 0;
  engine.handle({ type: 'init', config: { voxelSize: H, threads: 1, memoryMB: 256, params: { ...DEFAULT_PARAMS } } }, t);
  return {
    engine,
    messages,
    now: () => t,
    tick: (ms: number) => {
      for (let k = 0; k < ms / 16; k++) {
        t += 16;
        engine.tick(t);
      }
    },
  };
}

function meshesOf(messages: WorkerMessage[]): ChunkMesh[] {
  return messages.flatMap((m) => (m.type === 'chunkMeshes' ? m.meshes : []));
}

function eventsOf(messages: WorkerMessage[]): EngineEvent[] {
  return messages.flatMap((m) => (m.type === 'events' ? m.list : []));
}

test('engine: rooms world loads, meshes and reports stats', () => {
  const { engine, messages, tick } = runEngine();
  engine.handle({ type: 'loadProcedural', kind: 'rooms', seed: 7 }, 0);
  const tex = messages.find((m) => m.type === 'textures');
  assert.ok(tex && tex.type === 'textures' && tex.list.length >= 5);
  for (const t of tex.list) assert.equal(t.rgba.byteLength, t.width * t.height * 4);
  const ready = messages.find((m) => m.type === 'ready');
  assert.ok(ready && ready.type === 'ready');
  assert.ok(ready.info.voxelCount > 100_000);
  const w = engine.world!;
  // Spawn must be standing space: two metres of air above the feet.
  const g = w.toGrid(ready.info.spawn.pos);
  for (let dz = 0; dz < 16; dz++) assert.equal(w.get(Math.floor(g[0]), Math.floor(g[1]), Math.floor(g[2]) + dz), AIR);
  tick(20_000);
  const meshes = meshesOf(messages);
  assert.ok(meshes.length > 50, `${meshes.length} chunk meshes`);
  for (const m of meshes.slice(0, 40)) assertWindingAndIndices(m);
  const stats = messages.filter((m) => m.type === 'stats');
  assert.ok(stats.length > 0);
  const last = stats.at(-1)!;
  assert.ok(last.type === 'stats' && last.stats.meshQueue === 0 && last.stats.chunks > 50);
});

test('engine: raycast/collide answer by id; carve removes voxels and remeshes', () => {
  const { engine, messages, tick } = runEngine();
  engine.handle({ type: 'loadProcedural', kind: 'rooms', seed: 7 }, 0);
  tick(20_000);
  const ready = messages.find((m) => m.type === 'ready')!;
  assert.ok(ready.type === 'ready');
  const eye: Vec3 = [ready.info.spawn.pos[0], ready.info.spawn.pos[1], 1.6];
  engine.handle({ type: 'raycast', id: 42, origin: eye, dir: [0, 0, -1], maxDist: 50 });
  const rr = messages.at(-1)!;
  assert.ok(rr.type === 'raycastResult' && rr.id === 42 && rr.hit !== null);
  assert.ok(Math.abs(rr.hit.pos[2]) < 1e-6);
  engine.handle({ type: 'collide', id: 7, min: [eye[0] - 0.3, eye[1] - 0.3, 0.01], max: [eye[0] + 0.3, eye[1] + 0.3, 1.76], move: [0, 0, -0.5] });
  const cr = messages.at(-1)!;
  assert.ok(cr.type === 'collideResult' && cr.id === 7 && cr.onGround);
  const before = engine.world!.solidCount;
  const n = messages.length;
  engine.handle({ type: 'carve', pos: rr.hit.pos, radius: 0.3 });
  assert.ok(engine.world!.solidCount < before);
  tick(100);
  assert.ok(meshesOf(messages.slice(n)).length >= 1, 'the carved chunk is re-sent');
});

test('engine: shooting the bridge columns detaches the bridge', () => {
  const { engine, messages, tick, now } = runEngine();
  engine.handle({ type: 'loadProcedural', kind: 'rooms', seed: 7 }, 0);
  tick(20_000);
  const w = engine.world!;
  // Room (2,0) holds the bridge; its two columns are the only support.
  const m = (x: number): number => Math.round(x / H);
  const R = m(16);
  const W = m(0.5);
  const roomX = (i: number): number => W + i * (R + W);
  const len = R - 2 * m(1.5);
  const bx = roomX(2) + m(1.5);
  const by = roomX(0) + (R - m(2)) / 2;
  for (const f of [1 / 3, 2 / 3]) {
    const cx = bx + Math.round(len * f);
    const cy = by + m(1);
    const p = w.voxelCenter(cx, cy, 3 + m(1.5));
    engine.handle({ type: 'blast', pos: p, radius: 0.6, energy: 1e6 }, now());
    tick(50);
  }
  tick(100);
  const detached = eventsOf(messages).filter((e): e is DetachedEvent => e.kind === 'detached');
  assert.ok(detached.length >= 1, 'bridge detached');
  const big = detached.reduce((a, b) => (a.voxels > b.voxels ? a : b));
  assert.ok(big.voxels > 1000, `bridge island has ${big.voxels} voxels`);
  assertWindingAndIndices(big.mesh);
  const events = eventsOf(messages);
  assert.ok(events.some((e) => e.kind === 'impact'));
  assert.ok(events.some((e) => e.kind === 'crack'));
  // A rigid piece: it falls, lands (the landing impact arrives later) and stays as rubble.
  assert.equal(big.rigid, true);
  const impactsBefore = events.filter((e) => e.kind === 'impact').length;
  tick(3000);
  const impactsAfter = eventsOf(messages).filter((e) => e.kind === 'impact').length;
  assert.ok(impactsAfter > impactsBefore, 'landing impact');
  const poses = messages.flatMap((m) => (m.type === 'debris' ? [m.poses] : [])).at(-1);
  assert.ok(poses && poses.length % DEBRIS_STRIDE === 0, 'packed debris poses');
  let rest: number[] | null = null;
  for (let o = 0; o < poses.length; o += DEBRIS_STRIDE) if (poses[o] === big.id) rest = [...poses.subarray(o, o + DEBRIS_STRIDE)];
  assert.ok(rest !== null && rest[3]! < big.centroid[2] - 1 && rest[8] === 1, 'the bridge came down and is still there');
  const n = messages.length;
  tick(2000);
  assert.ok(!messages.slice(n).some((m) => m.type === 'debris'), 'resting rubble is not re-sent');
  const stats = messages.filter((m) => m.type === 'stats').at(-1);
  assert.ok(stats && stats.type === 'stats' && stats.stats.pieces >= 1 && stats.stats.awakePieces === 0, 'rubble at rest');
  assert.ok(stats.stats.detachedPieces >= 1 && stats.stats.detachedVoxels > 1000);
});

test('engine: debug view change re-sends meshes with debug bytes', () => {
  const { engine, messages, tick } = runEngine();
  engine.handle({ type: 'loadProcedural', kind: 'tower', seed: 3 }, 0);
  tick(20_000);
  const n = messages.length;
  engine.handle({ type: 'setParams', params: { ...DEFAULT_PARAMS, debugView: DebugView.Utilization } });
  tick(20_000);
  const resent = meshesOf(messages.slice(n));
  assert.ok(resent.length > 20);
  let hot = 0;
  for (const mesh of resent) {
    for (let i = 0; i < mesh.vertexCount; i += 7) if (decodeVertex(mesh.vertices, i).debug > 128) hot++;
  }
  assert.ok(hot > 0, 'some vertices show high utilization');
});

test('engine: fragments view gives the voxels fragment ids 1..254', () => {
  const { engine, messages, tick } = runEngine();
  engine.handle({ type: 'loadProcedural', kind: 'tower', seed: 3 }, 0);
  tick(20_000);
  const n = messages.length;
  engine.handle({ type: 'setParams', params: { ...DEFAULT_PARAMS, debugView: DebugView.Fragments } });
  tick(20_000);
  const ids = new Set<number>();
  for (const mesh of meshesOf(messages.slice(n))) {
    for (let i = 0; i < mesh.vertexCount; i += 5) ids.add(decodeVertex(mesh.vertices, i).debug);
  }
  assert.ok(ids.size > 50, `${ids.size} distinct fragment ids`);
  assert.ok(!ids.has(255));
});

test('engine: loadWad on the mock reports an error and falls back', () => {
  const { engine, messages } = runEngine();
  const bogus = new Uint8Array([80, 87, 65, 68, 0, 0, 0, 0, 12, 0, 0, 0]).buffer; // "PWAD", 0 lumps
  engine.handle({ type: 'loadWad', buffer: bogus, map: 'MAP01', options: { mode: 'rock', shellVoxels: 8, bake: false } });
  const err = messages.find((m) => m.type === 'error');
  assert.ok(err && err.type === 'error' && !err.fatal && err.command === 'loadWad');
  assert.ok(messages.some((m) => m.type === 'ready'));
});

test('padded chunk copy matches world.get', () => {
  const w = flatWorld();
  w.fillBox(30, 30, 30, 34, 34, 34, Block.Brick); // straddles chunk borders
  const buf = new Uint8Array(PADDED_CHUNK_VOLUME);
  const ci = w.chunkIndex(0, 0, 0);
  const block = fillPaddedChunk(w, ci, buf);
  const P = 34;
  for (let z = -1; z <= 32; z++)
    for (let y = -1; y <= 32; y++)
      for (let x = -1; x <= 32; x++) assert.equal(buf[x + 1 + P * (y + 1 + P * (z + 1))], w.get(x, y, z));
  assert.deepEqual(block.grid, [0, 0, 0]);
});

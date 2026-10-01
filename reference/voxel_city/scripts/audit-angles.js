#!/usr/bin/env node
/**
 * Angles audit: what the oriented parts round a point would cost the
 * physics of structvox (validate/angles.js, ANGLED_WORLD_PLAN.md §5.3).
 *   node scripts/audit-angles.js [--cx m] [--cy m] [--size m] [--preset id] [--variant v]
 * Parts per chunk, parts resident within 96 m (the budget: ~6-8), chunks
 * their boxes meet, overlaps between parts (Rule A), reach from the home
 * chunk (≤ 4) and the angles in use.
 */
import { createWorld } from "../src/engine/world/createWorld.js";
import { worldConfigFromArgs } from "./lib/world.js";
import { auditAngles } from "../src/engine/validate/angles.js";
import { YAWS, PITCHES, yawVector } from "../src/engine/core/placement.js";
import { PART_REACH } from "../src/engine/world/parts.js";

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, arr) => (a.startsWith("--") ? [...acc, [a.slice(2), arr[i + 1]]] : acc), []),
);
const world = createWorld(worldConfigFromArgs(args));
const cx = Number(args.cx ?? 0) * 8;
const cy = Number(args.cy ?? 0) * 8;
const half = (Number(args.size ?? 1200) * 8) / 2;
const rect = { x0: cx - half, y0: cy - half, x1: cx + half, y1: cy + half };
const t0 = performance.now();
const a = auditAngles(world, rect);
const ang = world.config.world.angles;

// (degrees for the report only: the tables themselves are exact rationals)
const yawDeg = (i) => {
  // (a product of two table yaws: i = yaw + 132 yaw2)
  const y = yawVector(i % YAWS.length, Math.floor(i / YAWS.length));
  return ((Math.atan2(y.s, y.c) * 180) / Math.PI + 360) % 360;
};
const grade = (i) => (Math.sign(i) * PITCHES[Math.abs(i)].s) / PITCHES[Math.abs(i)].c;
const hist = (m, label) =>
  [...m.entries()]
    .sort((p, q) => p[0] - q[0])
    .map(([k, n]) => `${label(k)} ${n}`)
    .join(", ") || "-";

console.log(`angles ${ang.enabled ? "on" : "off"} (yaws ${ang.yawSet}, pitches ${ang.pitchSet}, ${ang.partArea} m² per part, ${ang.maxPartsPerChunk} per chunk)`);
console.log(`area ${(a.areaM2 / 1e6).toFixed(2)} km²: ${a.parts} parts (${[...a.kinds].map(([k, n]) => `${k} ${n}`).join(", ") || "-"}), ${a.perArea.toFixed(2)} per ${ang.partArea} m²`);
console.log(`parts per chunk: most ${a.perChunk.max} (${a.perChunk.homes} home chunks)`);
console.log(`resident within 96 m (over ${a.resident.discs} discs): mean ${a.resident.mean.toFixed(2)}, p95 ${a.resident.p95}, most ${a.resident.max}   (budget ~6-8)`);
console.log(`chunks met by part boxes: ${a.partChunks}, where two parts meet: ${a.interfaceChunks}`);
console.log(`overlap between parts: ${a.overlap.voxels} voxels (${(a.overlap.fraction * 100).toFixed(3)}% of ${a.overlap.volume}), not owned by a priority: ${a.overlap.unowned}`);
console.log(`reach from the home chunk: most ${a.reach.max} chunks (limit ${PART_REACH})${a.reach.over.length ? `; over: ${a.reach.over.map((p) => `${p.key} (${p.reach})`).join(" ")}` : ""}`);
console.log(`yaws: ${hist(a.angles.yaw, (i) => `${yawDeg(i).toFixed(2)}°`)}`);
console.log(`pitches: ${hist(a.angles.pitch, (i) => `${(grade(i) * 100).toFixed(1)}%`)}`);
console.log(`rolls: ${hist(a.angles.roll, (i) => `${yawDeg(i).toFixed(2)}°`)}`);
console.log(`(${((performance.now() - t0) / 1000).toFixed(1)} s)`);

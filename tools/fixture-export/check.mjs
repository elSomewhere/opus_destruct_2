#!/usr/bin/env node
// Internal-consistency checker for svx-fixture-v1 fixtures.
//
//   node tools/fixture-export/check.mjs [DIR]      (default tests/fixtures/prototype)
//
// Uses only the fixture files (no prototype needed). Exit code 1 on failure.

import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { num } from './lib/json.mjs';

const TOOL_DIR = path.dirname(fileURLToPath(import.meta.url));
const dir = path.resolve(process.cwd(), process.argv[2] || path.join(TOOL_DIR, '../../tests/fixtures/prototype'));

const TOL = {
  equilibriumSum: 1e-5, // |sum reactions + sum loads| / max(1, |sum loads|)
  freeResidual: 1e-4, // max free-cell residual force / max cell load
  phi: 1e-9, // recomputed phi vs exported phi (relative)
  kSwap: 1e-12,
  analyticDiscrete: 1e-2, // measured / discrete-RBSM prediction (X strip, SS beam, fixed Y strip)
};

let failures = 0;
let warnings = 0;
const notes = [];
function fail(where, msg) { failures++; console.log(`  FAIL ${where}: ${msg}`); }
function warn(where, msg) { warnings++; console.log(`  warn ${where}: ${msg}`); }
function ok(cond, where, msg) { if (!cond) fail(where, msg); return cond; }

const rel = (a, b) => Math.abs(a - b) / Math.max(Math.abs(a), Math.abs(b), 1e-300);
const THR = ['openN', 'compN', 't1', 't2', 'b1', 'b2', 'tau'];

function phiOf(delta, onset) {
  const o = delta[0] >= 0 ? delta[0] / onset.openN : 0;
  const c = delta[0] < 0 ? -delta[0] / onset.compN : 0;
  const k = ['t1', 't2', 'b1', 'b2', 'tau'];
  let s = o * o + c * c;
  for (let i = 0; i < 5; i++) s += (delta[i + 1] / onset[k[i]]) ** 2;
  return Math.sqrt(s);
}

function beyondBox(delta, onset) {
  if (delta[0] >= 0 ? delta[0] > onset.openN : -delta[0] > onset.compN) return true;
  const k = ['t1', 't2', 'b1', 'b2', 'tau'];
  for (let i = 0; i < 5; i++) if (Math.abs(delta[i + 1]) > onset[k[i]]) return true;
  return false;
}

function sortedUnique(ids) {
  for (let i = 1; i < ids.length; i++) if (!(ids[i] > ids[i - 1])) return false;
  return true;
}

function checkModel(f, where) {
  const m = f.model;
  const cellById = new Map(m.cells.map((c) => [c.id, c]));
  const supById = new Map(m.supports.map((s) => [s.id, s]));
  ok(sortedUnique(m.cells.map((c) => c.id)), where, 'cell ids not strictly increasing');
  ok(sortedUnique(m.edges.map((e) => e.id)), where, 'edge ids not strictly increasing');
  ok(sortedUnique(m.supports.map((s) => s.id)), where, 'support ids not strictly increasing');
  for (const c of m.cells) {
    ok(c.ijk.every(Number.isInteger), where, `cell ${c.id} ijk not integer`);
    ok(c.x0.every((v, i) => v === c.ijk[i] * m.cellSize), where, `cell ${c.id} x0 != ijk*cellSize`);
    ok(c.mass > 0 && c.inertia.length === 3 && c.inertia.every((v) => v > 0), where, `cell ${c.id} mass/inertia`);
    ok(!!m.tables.cellArchetypes[c.archetype], where, `cell ${c.id} archetype ${c.archetype} not in table`);
    ok(!!m.tables.materials[c.material], where, `cell ${c.id} material ${c.material} not in table`);
    const arch = m.tables.cellArchetypes[c.archetype];
    const mat = m.tables.materials[c.material];
    if (arch && mat) ok(rel(c.mass, mat.density * arch.gravityMassVolume) < 1e-12, where, `cell ${c.id} mass != density*gravityMassVolume`);
    if (c.support != null) ok(supById.get(c.support)?.cell === c.id, where, `cell ${c.id} support ${c.support} mismatch`);
  }
  for (const s of m.supports) {
    ok(cellById.get(s.cell)?.support === s.id, where, `support ${s.id} not referenced by its cell`);
    ok(!!m.tables.supportProfiles[s.profile], where, `support ${s.id} profile ${s.profile} not in table`);
  }
  for (const e of m.edges) {
    const a = cellById.get(e.a);
    const b = cellById.get(e.b);
    if (!ok(a && b, where, `edge ${e.id} endpoints missing`)) continue;
    const d = [0, 1, 2].map((i) => b.ijk[i] - a.ijk[i]);
    ok(d.every((v, i) => v === (i === e.axis ? 1 : 0)), where, `edge ${e.id} is not a +axis face pair`);
    ok(!!m.tables.interfaceProfiles[e.profile], where, `edge ${e.id} profile ${e.profile} not in table`);
  }
  for (const [axis, fr] of Object.entries(m.edgeFrames)) {
    const n = fr.n; const t1 = fr.t1; const t2 = fr.t2;
    const cross = [n[1] * t1[2] - n[2] * t1[1], n[2] * t1[0] - n[0] * t1[2], n[0] * t1[1] - n[1] * t1[0]];
    ok(n.every((v, i) => Math.abs(v - (i === Number(axis) ? 1 : 0)) < 1e-12), where, `edgeFrames[${axis}].n is not e_axis`);
    ok(cross.every((v, i) => Math.abs(v - t2[i]) < 1e-12), where, `edgeFrames[${axis}] not right-handed (n x t1 != t2)`);
  }
  return { cellById };
}

function checkArchetypes(f, where) {
  const variants = f.variants;
  const base = variants.prototype?.interfaceArchetypes || {};
  for (const [vname, v] of Object.entries(variants)) {
    const w = `${where}/${vname}`;
    for (const e of f.model.edges) ok(!!v.interfaceArchetypes[e.archetype], w, `edge ${e.id} archetype missing`);
    for (const [id, a] of Object.entries(v.interfaceArchetypes)) {
      ok(a.k0.length === 6 && a.k0.every((k) => k > 0), w, `${id} k0 not positive`);
      for (const k of THR) ok(num(a.onset[k]) > 0 && num(a.onset[k]) < num(a.break[k]), w, `${id} onset.${k} !< break.${k}`);
      ok(a.forceCap6.every((c) => c >= 0) && a.compressionCap >= 0, w, `${id} negative capacity`);
      ok(a.plasticFraction === 0, w, `${id} plasticFraction != 0`);
      const b = base[id];
      if (!b || vname === 'prototype') continue;
      const isY = a.directionWeights[1] === 1;
      const fixY = vname === 'fixY' || vname === 'fixYJ';
      const fixJ = vname === 'fixJ' || vname === 'fixYJ';
      for (const i of [0, 1, 2]) ok(a.k0[i] === b.k0[i], w, `${id} k0[${i}] changed by a section-only variant`);
      if (fixY && isY) {
        ok(rel(a.k0[3], b.k0[4]) < TOL.kSwap && rel(a.k0[4], b.k0[3]) < TOL.kSwap, w, `${id} fixY is not a k0[3]<->k0[4] swap`);
      } else {
        ok(a.k0[3] === b.k0[3] && a.k0[4] === b.k0[4], w, `${id} bending k0 changed unexpectedly`);
      }
      if (fixJ) ok(a.k0[5] <= b.k0[5] * (1 + 1e-12), w, `${id} St-Venant J larger than polar J`);
      else ok(a.k0[5] === b.k0[5], w, `${id} torsion k0 changed unexpectedly`);
    }
  }
}

function checkStatic(f, where, cellById) {
  for (const [vname, v] of Object.entries(f.variants)) {
    const w = `${where}/${vname}/static`;
    const s = v.static;
    if (!ok(s, w, 'missing static block')) continue;
    ok(['converged', 'not_converged', 'no_anchored_cells'].includes(s.status), w, `bad status ${s.status}`);
    ok(s.cells.u.length === f.model.cells.length && s.cells.theta.length === f.model.cells.length, w, 'cell arrays misaligned');
    ok(s.edges.force6.length === f.model.edges.length && s.edges.phi.length === f.model.edges.length, w, 'edge arrays misaligned');
    ok(s.supports.reaction6.length === f.model.supports.length, w, 'support arrays misaligned');
    s.cells.u.forEach((u, i) => ok((u == null) === (s.cells.theta[i] == null), w, `cell ${f.model.cells[i].id} u/theta null mismatch`));
    if (s.status !== 'converged') {
      notes.push(`${w}: status ${s.status}`);
      continue;
    }
    // Recompute the external load of anchored cells from the model alone.
    const g = f.scene.gravity;
    const ext = [0, 0, 0];
    let maxLoad = 0;
    f.model.cells.forEach((c, i) => {
      if (s.cells.u[i] == null) return;
      const fe = [c.appliedForce?.[0] || 0, c.appliedForce?.[1] || 0, c.mass * g + (c.appliedForce?.[2] || 0)];
      for (let k = 0; k < 3; k++) ext[k] += fe[k];
      maxLoad = Math.max(maxLoad, Math.hypot(...fe));
    });
    const eq = s.equilibrium;
    ok(ext.every((x, k) => Math.abs(x - eq.totalExternalForce[k]) <= 1e-9 * Math.max(1, Math.abs(x))), w,
      `totalExternalForce ${eq.totalExternalForce} != recomputed ${ext}`);
    const react = [0, 0, 0];
    for (const r of s.supports.reaction6) if (r) for (let k = 0; k < 3; k++) react[k] += r[k];
    const sumErr = Math.max(...[0, 1, 2].map((k) => Math.abs(react[k] + ext[k])));
    ok(sumErr <= TOL.equilibriumSum * Math.max(1, Math.hypot(...ext)), w, `sum reactions + loads = ${sumErr} N`);
    const freeRel = eq.maxFreeCellResidualForce / Math.max(1, maxLoad);
    ok(freeRel <= TOL.freeResidual, w, `free-cell residual ${eq.maxFreeCellResidualForce} N (${freeRel.toExponential(2)} of max cell load)`);
    // phi / force6 / k0 / onset consistency (damage frozen at 0 => s(d) = 1).
    let maxPhi = 0;
    let beyond = 0;
    let minDz = null;
    s.cells.u.forEach((u) => { if (u) minDz = minDz == null ? u[2] : Math.min(minDz, u[2]); });
    f.model.edges.forEach((e, i) => {
      const f6 = s.edges.force6[i];
      if (!f6) { ok(s.edges.phi[i] == null, w, `edge ${e.id} phi without force`); return; }
      const a = v.interfaceArchetypes[e.archetype];
      const delta = f6.map((x, k) => x / a.k0[k]);
      const phi = phiOf(delta, a.onset);
      ok(Math.abs(phi - s.edges.phi[i]) <= TOL.phi * Math.max(1, phi), w, `edge ${e.id} phi ${s.edges.phi[i]} != recomputed ${phi}`);
      maxPhi = Math.max(maxPhi, s.edges.phi[i]);
      if (beyondBox(delta, a.onset)) beyond++;
    });
    ok(maxPhi === s.maxPhi, w, `maxPhi ${s.maxPhi} != max(phi) ${maxPhi}`);
    ok(beyond === s.edgesBeyondOnset, w, `edgesBeyondOnset ${s.edgesBeyondOnset} != recomputed ${beyond}`);
    ok(s.elasticRegime === (beyond === 0), w, 'elasticRegime flag inconsistent');
    if (s.maxPhi < 1) ok(s.elasticRegime, w, 'maxPhi < 1 but not elastic');
    ok(s.probes.minDz === minDz, w, `probe minDz ${s.probes.minDz} != ${minDz}`);
  }
}

function checkOutcome(f, where) {
  const cellIds = new Set(f.model.cells.map((c) => c.id));
  const edgeIds = new Set(f.model.edges.map((e) => e.id));
  for (const [vname, v] of Object.entries(f.variants)) {
    const w = `${where}/${vname}/outcome`;
    const o = v.outcome;
    if (!ok(o, w, 'missing outcome block')) continue;
    ok(o.initialCellCount === f.model.cells.length, w, 'initialCellCount != model cells');
    ok(sortedUnique(o.aliveCellIds) && o.aliveCellIds.every((id) => cellIds.has(id)), w, 'aliveCellIds not sorted/unique/known');
    ok(o.removedCount === o.initialCellCount - o.aliveCellIds.length, w, 'removedCount inconsistent');
    const alive = new Set(o.aliveCellIds);
    const removed = [...o.deletedCellIds, ...o.blastRemovedCellIds];
    ok(new Set(removed).size === removed.length, w, 'a cell was removed twice');
    ok(removed.length === o.removedCount && removed.every((id) => cellIds.has(id) && !alive.has(id)), w, 'deleted + blast-removed != removed set');
    const rupt = new Set(o.rupturedEdgeIds);
    ok(rupt.size === o.rupturedEdgeIds.length && o.rupturedEdgeIds.every((id) => edgeIds.has(id)), w, 'rupturedEdgeIds not unique/known');
    ok(o.blastFracturedEdgeIds.every((id) => edgeIds.has(id) && !rupt.has(id)), w, 'blast-fractured edges overlap ruptures');
    const sumR = o.passLog.reduce((s, p) => s + p.ruptured, 0);
    const sumD = o.passLog.reduce((s, p) => s + p.detachedDeleted, 0);
    ok(sumR === o.rupturedEdgeIds.length, w, `sum pass ruptures ${sumR} != rupturedEdgeIds ${o.rupturedEdgeIds.length}`);
    ok(sumD === o.deletedCellIds.length, w, `sum pass deletions ${sumD} != deletedCellIds ${o.deletedCellIds.length}`);
    ok(o.passLog.every((p, i) => i === 0 || p.pass === o.passLog[i - 1].pass + 1), w, 'passLog not consecutive');
    if (o.converged) {
      const last = o.passLog[o.passLog.length - 1];
      ok(last?.quiet === true && last.pass === o.passes, w, 'converged but last logged pass not quiet / passes mismatch');
      ok(o.totalPassesRun === o.passes + o.trailingQuietPasses, w, 'pass bookkeeping inconsistent');
    } else {
      notes.push(`${w}: NOT converged after ${o.passes} passes`);
    }
    if (!o.allPassesNewtonConverged) notes.push(`${w}: some pass had a non-converged anchored Newton solve (${o.nonConvergedAnchoredComponentSolves} solves)`);
    ok(o.finalU.length === o.aliveCellIds.length, w, 'finalU misaligned');
    for (const [id] of o.damagedEdges) ok(edgeIds.has(id) && !rupt.has(id), w, `damaged edge ${id} unknown or ruptured`);
  }
}

function checkFeel(f, where) {
  if (!f.feel) { warn(where, 'no feel block'); return; }
  const groups = [['feel', f.feel.runs]];
  if (f.feel.appStreamed) groups.push(['feel.appStreamed', f.feel.appStreamed.runs]);
  for (const [label, group] of groups) for (const [policy, runs] of Object.entries(group)) {
    const phases = [['settle', runs.settle], ...Object.entries(runs.blasts || {}).map(([id, b]) => [`blast:${id}`, b])];
    for (const [name, ph] of phases) {
      const w = `${where}/${label}/${policy}/${name}`;
      const s = ph.series;
      const cols = ['tick', 'maxDisp', 'minDz', 'maxDispAnchored', 'minDzAnchored', 'awake', 'alive', 'anchored', 'ruptures', 'deleted'];
      ok(cols.every((c) => Array.isArray(s[c]) && s[c].length === s.samples), w, 'series columns misaligned');
      ok(s.samples <= f.feel.protocol.maxSamples, w, `too many samples ${s.samples}`);
      ok(s.tick.every((t, i) => i === 0 || t > s.tick[i - 1]), w, 'ticks not increasing');
      ok(s.tick[s.tick.length - 1] === ph.summary.ticks, w, 'last tick != summary.ticks');
      ok(s.ruptures.every((r, i) => i === 0 || r >= s.ruptures[i - 1]), w, 'cumulative ruptures decrease');
      ok(s.alive[s.alive.length - 1] === ph.summary.finalAlive, w, 'final alive mismatch');
      ok(['all_asleep', 'max_ticks'].includes(ph.summary.stopReason), w, 'bad stopReason');
      if (ph.summary.stopReason === 'all_asleep') ok(s.awake[s.awake.length - 1] === 0, w, 'stopped asleep but awake > 0');
      ok(ph.summary.peakDisp >= Math.max(...s.maxDisp) - 1e-15, w, 'peakDisp below a sample');
      if (name.startsWith('blast')) ok(Array.isArray(ph.center) && ph.radius > 0, w, 'blast center/radius missing');
    }
  }
}

function checkAnalytic(f, where) {
  for (const [vname, v] of Object.entries(f.variants)) {
    const a = v.analytic;
    if (!a) continue;
    const w = `${where}/${vname}/analytic`;
    ok(a.measuredDeflection > 0, w, 'measured deflection not positive');
    if (a.kind === 'plate_ss_center_load') {
      notes.push(`${w}: plate centre deflection / Kirchhoff = ${a.ratios.measuredOverKirchhoff.toFixed(4)}`);
      continue;
    }
    const r = a.ratios.measuredOverDiscreteRBSM;
    notes.push(`${w}: measured / discrete-RBSM = ${r.toFixed(6)}, / Timoshenko = ${a.ratios.measuredOverTimoshenko.toFixed(6)}`);
    ok(Math.abs(r - 1) <= TOL.analyticDiscrete, w, `measured/discrete = ${r}`);
  }
}

function main() {
  const indexPath = path.join(dir, 'index.json');
  if (!fs.existsSync(indexPath)) {
    console.log(`no index.json in ${dir}`);
    process.exit(1);
  }
  const index = JSON.parse(fs.readFileSync(indexPath, 'utf8'));
  ok(index.schema === 'svx-fixture-v1' && index.kind === 'index', 'index', 'bad index schema');
  const listed = new Set(index.scenes.map((s) => s.file));
  for (const file of fs.readdirSync(dir)) {
    if (file.endsWith('.json') && file !== 'index.json' && !listed.has(file)) warn('index', `${file} not listed in index.json`);
  }
  for (const entry of index.scenes) {
    const where = entry.id;
    const p = path.join(dir, entry.file);
    if (!ok(fs.existsSync(p), where, 'file missing')) continue;
    const text = fs.readFileSync(p, 'utf8');
    ok(crypto.createHash('sha256').update(text).digest('hex') === entry.sha256, where, 'sha256 mismatch vs index.json');
    ok(Buffer.byteLength(text) === entry.bytes, where, 'size mismatch vs index.json');
    const f = JSON.parse(text);
    const before = failures;
    ok(f.schema === 'svx-fixture-v1', where, 'bad schema');
    ok(f.scene?.id === entry.id, where, 'scene id mismatch');
    for (const k of ['scene', 'prototype', 'conventions', 'model', 'variants']) ok(f[k], where, `missing block ${k}`);
    ok(!!f.variants.prototype, where, 'missing prototype variant');
    ok((f.skipped || []).length === 0, where, `skipped stages: ${JSON.stringify(f.skipped)}`);
    const { cellById } = checkModel(f, where);
    checkArchetypes(f, where);
    checkStatic(f, where, cellById);
    checkOutcome(f, where);
    checkFeel(f, where);
    checkAnalytic(f, where);
    const p0 = f.variants.prototype;
    console.log(`${failures === before ? 'ok  ' : 'FAIL'} ${entry.id.padEnd(32)} static ${p0.static.status.padEnd(17)} outcome ${p0.outcome.converged ? 'converged' : 'NOT-CONV '} newton-all ${p0.outcome.allPassesNewtonConverged ? 'y' : 'n'} alive ${p0.outcome.aliveCellIds.length}/${p0.outcome.initialCellCount}`);
  }
  console.log('\nnotes:');
  for (const n of notes) console.log(`  ${n}`);
  console.log(`\n${index.scenes.length} fixtures, ${failures} failures, ${warnings} warnings`);
  if (failures > 0) process.exitCode = 1;
}

main();

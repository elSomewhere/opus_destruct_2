# Behavioural baseline: the engine against its structural reference

The structural-integrity and destruction behaviour of the engine has a reference: the
`oriented_1` branch (`e5026c0`), before vehicles, crumpling, plastic hinges, articulations and
the optimizations since. Everything added since is either a new mechanism (it does nothing where
it is not used) or a change of how the old ones behave. Each change of behaviour is a **switch**
(a world tunable, on by default), and with all of them off the engine reproduces the reference
**bit for bit**: the same world, the same pieces at the same poses with the same velocities,
the same bonds broken, tick after tick, in every scenario below. What the engine does
differently by default is therefore exactly what the switches do, and nothing else.

## 1. The reference build

Build `oriented_1` with **clang**. Built with GCC it is wrong: its multigrid's kernels use
clang's `ext_vector_type`, which GCC ignores (a warning), so a GCC build solves structures with
broken smoother arithmetic. (`3a7fc32`, on the way to the cars branch, made those kernels
portable: `oriented_1` built with clang and `3a7fc32` built with GCC are bit-identical, which is
what makes the clang build the reference.)

```sh
git worktree add ../o1 e5026c0
git -C ../o1 apply "$PWD/tools/baseline/oriented_1-tools.patch"   # (its tools report as ours do)
cmake -S ../o1 -B ../o1/build-clang -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build ../o1/build-clang --target svx_engine_demo svx_env_bench -j
```

`oriented_1-tools.patch` touches the reference's two tools only, never what they run: they print
the world's hash, take `--tune NAME=VALUE` and (the env bench) `--archive-mb MB`, and trace every
tick's hash with `SVX_TRACE_HASH=1` (the demo also its cracks and pieces with
`SVX_TRACE_EVENTS=T`).

What is compared is `World::session_hash()`: the world's whole state - its grids' voxels and
bonds, every piece's voxels, pose and velocity, the joints, the clock. (`Game::session_hash`
adds the game's own state - movers and, since the cars branch, its vehicles and the player's
car - so it is not comparable across the two; both tools print the world's hash as `world`.)

## 2. The switches

| Tunable (`WorldConfig`) | On (default) | Off (the reference) | Since |
| --- | --- | --- | --- |
| `impact_penetration` | impacts remove material by energy density against its `penetration` resistance; a cut cuts steel and bars too ([`DAMAGE.md`](DAMAGE.md) §2) | carves and craters never remove ductile material | `d14cd84` |
| `restart_diverging_solves` | a structure solve whose residual diverges (many breaks at once) rebuilds its preconditioner at once | only after 120 iterations | `d14cd84` |
| `jointed_keep_identity` | a piece held by joints keeps its id through a split, on its largest part | its parts are new pieces | `d14cd84` |
| `rigid.piece_ccd` | pieces closing fast look ahead for each other too (speculative contacts): thin panels are not passed through | only the statics are looked ahead for | `d14cd84` |
| `plastic_hinges` | ductile sections failing in bending yield on a hinge ([`DAMAGE.md`](DAMAGE.md) §3) | they snap | `65c45a9`, `07e9d9f` |
| `spread_per_partner` | a piece's stress check spreads each partner's contact forces on their own | all of them pooled | `07e9d9f` |
| `load_trigger_gap` (6; reference: **0**) | a creeping load on a large structure is solved again at most every 6 ticks | at once, always | `75db2bd` |
| `design_in_place` | a streamed structure touched for the first time is designed in place, its solve going on from the design's | designed, then extracted again (a new structure, solved afresh) | `3963819` |
| `patch_cut_structures` | a structure cut by an event is patched where it was cut: a few chunks' work | extracted again whole about the cut | `3963819` |
| `evict_scan_ticks` (10; reference: **1**) | streaming's scan for what to evict (it walks every resident chunk) runs every 10 ticks, at once when a focus point moved 8 m, and every 30 ticks | every tick | `65c45a9` |
| `release_solvers` | a piece's fracture solver (its stiffness matrix and multigrid: most of a piece's memory) is released when it falls asleep, and over the pieces' budget the awake ones' too, the largest first, before any piece is culled; it is assembled afresh at its next check | kept while the piece lives: the budget culls pieces | `3ab0666` |
| `recheck_vacated` | where material leaves a grid (a piece comes loose, a shard turns to dust), what was next to it - edge to edge and corner to corner too - is checked for support: what held on to nothing else falls (a tree's leaves, with its trunk) | only what a carve or a blast cut is checked | `3ab0666` |
| `true_solve_work` | a structure's solver work is counted at its cost against `stress_work`: a multigrid's dense coarsest solve by its blocks, an assembly by the products and the factorization that build its multigrid | the coarsest solve by its unknowns (36 times its cost: a large structure got an iteration or two a tick), an assembly as 30 operations per matrix block | `3ab0666` |
| `rebuild_stale_only` | a solve slow to converge, or whose residual grows, rebuilds its preconditioner only when that makes another (a stale one, a small structure's block-Jacobi one), else restarts on it; a diverged iterate is never the next solve's start | rebuilt in any case (a large structure assembled again every tick, its solve getting nowhere), the iterate kept | `3ab0666` |
| `coarsen_dense_levels` | a stress solve's multigrid level grown dense (a large damaged structure's third or fourth) is coarsened once more, by its aggregates' rigid motions, and that solved densely (since this pass at any size: a dense level of a hundred nodes was factored, an assembly's most expensive part) | it is the coarsest: factored if it is small enough, else smoothed - eight sweeps of its dense rows a cycle, slow to converge | `3ab0666`, this pass |
| `reaggregate_levels` | a stress solve's multigrid level whose aggregates would hold fewer than two nodes on average (its couplings mostly under the strength threshold: a large irregular structure's second level) is aggregated again at half the threshold, up to three times | aggregated once: it barely coarsens, and its smoothed coarse level fills in (a damaged building's: 180 blocks a row - each assembly 200 ms and more, each cycle twice the work) | this pass |
| `shards_hold_together` | a fragment a blast or a punch tears out whole has its faces with the rest torn, and keeps its own: a later cut or shot breaks the shard as a piece | every face of its voxels is torn: a shard of loose voxels, all dust at the next cut | this pass |
| `cluster_cubes` (default **off**: a choice for the quality pass) | a structure of more than `cluster_nodes` fragments has them clustered in 1 m (2 m) cubes of its chunks, as was meant - large structures fail differently (the tower's side blast brings it down: 7966 bonds broken, not 32), and a large damaged building's solve has some 1.5 times the nodes and costs 2.5 times as much | the cell key loses the cell along x: clusters a chunk (4 m) long in x - cracks along x only at chunk edges, a building's strength by its orientation | this pass |
| `fair_solve_order` | the structures solving share `stress_work` from where it ran out the tick before - the first one it did not reach goes first - so a large structure slow to converge cannot hold the others back (a demolition's remnants waited up to 17 ticks to fall; served 66 times to 558 waits in its first 300 ticks) | by id every tick: the same structures first | this pass |
| `ensure_before_walk` | a streamed structure touched for the first time whose reach (the chunks a walk from it can get to) borders chunks not generated yet has them generated, and their neighbours, before it is walked: walked once (the drive city's first blast at a building: 1.86 s -> 1.33 s) | walked, then - meeting them - dropped, a ring of chunks about it generated, and walked again (and again, a ring at a time: a 16 m block, four walks) | this pass |
| `rigid.busy_hold` | busy mode (one substep a tick, fewer iterations) is decided once a tick for all its substeps, and holds until the collapse is under two thirds of both thresholds | decided again every substep, at the thresholds: a collapse at their edge switches between one substep and two, tick after tick | `3ab0666` |
| `rigid.warm_to_step` | the impulses contacts, wheels and joints start a substep from are scaled to its length (1/60 s after 1/120 s: twice them - what holds a resting load, a car on its springs) | as they were: a car dropped and kicked up at each switch of busy mode | `3ab0666` |

Set them with `--tune NAME=VALUE` on `svx_engine_demo` and `svx_env_bench`, `World::configure`,
`set_tunable` (C++, the C API, the web worker's settings panel). `tools/baseline/compare.sh
--parity` and `golden.sh`'s parity lines set all twenty-two.

**Content.** Three procedural levels changed as well: the steel members of the `yard` (the
greenhouse's and the shed's frames), the `angles` world (the braced portal) and the `machines`
world (the crane's jib) are `steel_section` since `65c45a9` - rolled sections smeared over their
voxels ([`DAMAGE.md`](DAMAGE.md) §1), not solid 12.5 cm steel. With those three lines of
`procgen/src/levels.cpp` back to `MaterialId::Steel`, those levels reproduce the reference too
(and so does the environment bench's fire, which burns the yard's house).

Everything else added since the reference is bit-neutral where it is not used: wheels,
crumpling, articulations and their fine steps, latches, the fragment memo of session grids, the
prefragment flood that crosses only where free voxels meet, the fragmenter's per-chunk seed
table, the mesher's bit-column face search.

## 3. Results

`tools/baseline/compare.sh <oriented_1 clang>/tools <this build>/tools --parity --threads 4`
(the three levels' steel as in the reference, the world's hash at the end; each run's trace of
every tick's hash is the same too, where it was traced):

| Scenario (`svx_engine_demo --world ...`) | Reference world hash | Reproduced | Pieces | Bonds broken | Voxels detached |
| --- | --- | --- | --- | --- | --- |
| tower, side, 5 s | `640880863432ffb9` | yes | 9 | 32 | 502 |
| tower, core, 5 s | `e397f365ce187a22` | yes | 1401 | 9053 | 202347 |
| tower, pillars, turned 30 degrees, 5 s | `28e0ebdbc1b812cc` | yes | 210 | 1020 | 201261 |
| tower, all, 5 s | `4171b4e97aa5b103` | yes | 1537 | 9506 | 201046 |
| tower, rockets, 5 s | `407b0eb589d0140d` | yes | 1417 | 8779 | 202448 |
| tower, pillars, 8 s | `ae2f64eb988195af` | yes | 1481 | 8834 | 201558 |
| rooms, 5 s | `96bbf9affb47490b` | yes | 74 | 87 | 4002 |
| slab, 5 s | `5bed2317fe01ebed` | yes | 80 | 289 | 6136 |
| bridge, 5 s | `4a2a3bdec467cf82` | yes | 197 | 1272 | 27091 |
| chimney, 5 s | `075130665ee44277` | yes | 279 | 4865 | 15009 |
| yard, 8 s (solid steel) | `383a926a8f147058` | yes | 829 | 6972 | 61724 |
| machines, 12 s (solid steel) | `e8402d57de4e6404` | yes | 50 | 719 | 12221 |
| angles, 6 s (solid steel) | `f987b58c8796b7bd` | yes | 745 | 6421 | 89115 |
| city (streamed), 6 s | `085ae436d28344f8` | yes | 2568 | 22622 | 281824 |
| `svx_env_bench`: fire (the yard's house burning, 2 min; solid steel) | `12a6324086fdf6a5` | yes | | | |
| `svx_env_bench`: flood (a reservoir breached, 30 s) | `c1db95d89a6fb355` | yes | | | |
| `svx_env_bench`: city (the streamed city crossed for 90 s, blasts, fires and water), memory budgets unbound | `c723934e3081cc9d` | yes: all 5400 ticks (traced) | | | |

Every one of them reproduced on 4 threads natively; the short ones (side, rooms, slab,
chimney) under Node from the WASM build on one thread as well.
Under Node (Emscripten 6.0.10, 4 threads) every line of `tools/baseline/golden.sh` matches
too but the `angles` world's (`d8bee4389c71eb64` against `94a83a4f622d8e7e`), before the merge
as after it: that level places its grids with the platform's `sin`, `cos` and `atan2`
(`procgen/src/levels.cpp`), which differ in the last bit between glibc and musl. The engine
itself is not involved, and the city generator's numerics are its own (V8's and fdlibm's,
docs/CITY.md); moving the level to `dm::` would change its golden hash, so it waits for a step
that re-baselines.

**The streamed city crossed with fire and water** runs into the memory budgets: the change
archive's (`StreamConfig::archive_mb`, 64 MB) and the pieces' (`memory.piece_mb`, 256 MB, at the
cap of 3000 pieces). What the engine keeps weighs more now - a piece in memory has the area each
of its contact samples stands for (crumpling's), the archive's records a piece's speed limit and
a group's wheels - so with the same budgets the two builds cull other pieces, and forget other
regions, at other ticks. Traced: with the default budgets the crossing agrees for (at least) its
first 970 ticks and its end differs (the reference ends at `b3ba4eb6c8eb7bdd`); with the archive
unbound it agrees for its first 2269 ticks, where the pieces' budget first culls at the cap -
68 pieces more, the rest of the tick the same. With every budget out of the way
(`svx_env_bench --scenario city --archive-mb 0 --tune memory.piece_mb=8192 --tune
memory.structure_mb=8192 --tune memory.cache_mb=8192 --tune memory.fragment_cache_mb=8192`, on
both builds) the whole crossing is the same, tick for tick. The budgets are the lossy knobs of a
long session ([`CORE.md`](CORE.md) §8).

### The engine as it is

With the switches on - the engine's defaults, and the levels' steel sections - the same
scenarios, reference first (`compare.sh` without `--parity`; 3 threads, one of the machine's
four cores busy elsewhere, so the wall times compare with each other, not with a quiet machine):

| Scenario | Pieces | Bonds broken | Voxels detached | Wall (s) |
| --- | --- | --- | --- | --- |
| tower, side, 5 s | 9 / 9 | 32 / 32 | 502 / 502 | 0.4 / 0.4 |
| tower, core, 5 s | 1401 / 1407 | 9053 / 8696 | 202347 / 202347 | 10.3 / 5.9 |
| tower, pillars, turned 30 degrees, 5 s | 210 / 2028 | 1020 / 11664 | 201261 / 201582 | 4.2 / 10.7 |
| tower, all, 5 s | 1537 / 2048 | 9506 / 11893 | 201046 / 201051 | 16.6 / 14.1 |
| tower, rockets, 5 s | 1417 / 2123 | 8779 / 12219 | 202448 / 202449 | 11.6 / 11.4 |
| tower, pillars, 8 s | 1481 / 2213 | 8834 / 12800 | 201558 / 201559 | 18.0 / 20.9 |
| rooms, 5 s | 74 / 72 | 87 / 83 | 4002 / 3827 | 0.7 / 0.3 |
| slab, 5 s | 80 / 71 | 289 / 299 | 6136 / 6662 | 0.6 / 0.4 |
| bridge, 5 s | 197 / 148 | 1272 / 1116 | 27091 / 29201 | 2.4 / 2.0 |
| chimney, 5 s | 279 / 273 | 4865 / 4676 | 15009 / 15009 | 3.3 / 2.3 |
| yard, 8 s | 829 / 898 | 6972 / 7173 | 61724 / 61658 | 14.4 / 13.8 |
| machines, 12 s | 50 / 50 | 719 / 665 | 12221 / 12221 | 1.6 / 0.9 |
| angles, 6 s | 745 / 909 | 6421 / 7560 | 89115 / 89864 | 12.8 / 9.4 |
| city (streamed), 6 s | 2568 / 3000 | 22622 / 27387 | 281824 / 281824 | 33.1 / 31.2 |

What comes down is the same (the voxels detached are within a few per cent everywhere); how it
breaks as it comes down is not. A collapsing tower breaks into some half as many pieces again:
a piece's stress check feels the bending its supports put in it, each partner's contacts spread
on their own (`spread_per_partner`: pooled, they cancel out), and its ductile sections yield on
hinges. The turned tower is the telling case: in 8 s the reference breaks a tower turned 30
degrees into 210 pieces where the same tower upright breaks into 1481; the engine breaks them
into 2120 and 2213 - a structure behaves the same whichever way its grid is turned. The cost
follows the pieces: faster where the scenario is the structure's work (patched structures, the
fragmenter's seed table, in-place design: the tower's core 10.3 to 5.9 s, the machines 1.6 to
0.9 s), slower where there are many more pieces to simulate (the turned tower).

## 4. Gates

- `tools/baseline/golden.sh build/native-release/tools` runs the scenarios of
  `tools/baseline/golden.txt` and checks their world hashes: *parity* lines (the reference's
  own hashes, reproduced with the switches off: never rewritten) and *engine* lines (the engine
  as it is: a change of behaviour shows here, and is pinned again with `--update` in the change
  that makes it, deliberately). CI runs it natively on every push, and under Node on the WASM
  build (the same hashes: the engine is bit-identical on every platform).
- `tools/baseline/compare.sh REF NEW [--parity] [--quick] [--no-env] [--new-tune NAME=VALUE]`
  compares two builds scenario by scenario: hashes, outcome counts, timings.
- `svx_env_bench --budget-ms MS` fails a run whose mean tick is slower: CI holds the fire and
  the flood to 60 ms (they run in some 5 ms on a quiet 4-core machine, up to 16 with other work
  on it; the reference's environment took 95 - 120 ms) - a gate for a regression, not for a
  noisy runner. The soak
  (`svx_soak --check`: the drive city for minutes, its memory must level off) and the
  sanitizers (ASan with UBSan and LeakSanitizer on every suite, TSan on the thread pool's) run
  in CI too.
- `SVX_TRACE_HASH=1` makes both tools print every tick's world hash, and `SVX_TRACE_EVENTS=T`
  the demo's cracks and pieces from tick T: two builds' traces show the tick where they part
  and what happened there (a structure's solve, a piece's id, a contact).

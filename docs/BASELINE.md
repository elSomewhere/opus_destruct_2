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
cmake -S ../o1 -B ../o1/build-clang -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build ../o1/build-clang --target svx_engine_demo svx_env_bench -j
```

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

Set them with `--tune NAME=VALUE` on `svx_engine_demo` and `svx_env_bench`, `World::configure`,
`set_tunable` (C++, the C API, the web worker's settings panel). `tools/baseline/compare.sh
--parity` and `golden.sh`'s parity lines set all ten.

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
| `svx_env_bench`: city (the streamed city crossed for 90 s, blasts, fires and water) | `b3ba4eb6c8eb7bdd` | its first 970 ticks (traced); not its end: see below | | | |

Every one of them reproduced on 4 threads natively; the short ones (side, rooms, slab,
chimney) under Node from the WASM build on one thread as well.

**The streamed city crossed with fire and water** agrees tick for tick for (at least) the first
970 of its 5400 ticks, and its end differs. The crossing fills the change archive's 64 MB
budget, and what the archive keeps is larger now (a piece's record has its speed limit, a
group's its wheels): which regions are forgotten when the budget is reached can differ. This
is being checked with an unbounded archive (`svx_env_bench --archive-mb 0`) on both builds.


## 4. Gates

- `tools/baseline/golden.sh build/native-release/tools` runs the scenarios of
  `tools/baseline/golden.txt` and checks their world hashes: *parity* lines (the reference's
  own hashes, reproduced with the switches off: never rewritten) and *engine* lines (the engine
  as it is: a change of behaviour shows here, and is pinned again with `--update` in the change
  that makes it, deliberately). CI runs it natively on every push, and under Node on the WASM
  build (the same hashes: the engine is bit-identical on every platform).
- `tools/baseline/compare.sh REF NEW [--parity] [--quick] [--no-env] [--new-tune NAME=VALUE]`
  compares two builds scenario by scenario: hashes, outcome counts, timings.
- `SVX_TRACE_HASH=1` makes both tools print every tick's world hash, and `SVX_TRACE_EVENTS=T`
  the demo's cracks and pieces from tick T: two builds' traces show the tick where they part
  and what happened there (a structure's solve, a piece's id, a contact).

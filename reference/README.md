# References

## `voxel_city/`

A snapshot of [`elSomewhere/voxel_city`](https://github.com/elSomewhere/voxel_city) at commit
`4ed8e162710579d95b49f0f00b8a152215a6f5ed` (2026-10-01, "Render lab: add weather and grit
looks"), taken with `git archive` (its `.cursor/`, `.vscode/`, `.github/`, `public/` and
`index.html` left out). It is the reference of the city generator's C++ port (`level/city/`,
[`docs/CITY.md`](../docs/CITY.md)): the port reproduces it bit for bit, stage by stage
(`tools/procgen_ref/`).

It is vendored rather than a git submodule: the repository is private, and CI's token cannot
fetch a private submodule of another repository.

- Never edited here. Porting a later change of voxel_city is a deliberate step: replace the
  snapshot, update this file, re-record the conformance stages it changes
  (`tools/procgen_ref/record.sh`), and port the change.
- Its own checks pass in the snapshot (Node 22):

  ```sh
  cd reference/voxel_city && npm ci
  node --test                              # 208 tests
  node scripts/golden.js --check           # the axis-aligned presets' golden record
  node scripts/golden.js --angled --check  # the angled presets'
  ```

  The conformance dumpers (`tools/procgen_ref/dump.mjs`) import its engine modules directly and
  need no `npm ci`.

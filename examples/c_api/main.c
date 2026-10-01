/* The physics core from plain C (svx/svx_core.h): a brick arch on rock, a blast at its foot,
 * ten seconds of simulation, the events and pieces read back. */
#include <stdio.h>
#include <stdlib.h>

#include "svx/svx_core.h"

int main(void) {
  /* 12 m x 2 m x 8 m of voxels (h = 0.125 m): a rock plate and two brick piers with a lintel */
  enum { NX = 96, NY = 16, NZ = 64 };
  uint8_t* v = calloc((size_t)NX * NY * NZ, 1);
  const uint8_t rock = svxc_vox(SVXC_ROCK, 1), brick = svxc_vox(SVXC_MASONRY, 0);
  for (int x = 0; x < NX; ++x)
    for (int y = 0; y < NY; ++y)
      for (int z = 0; z < NZ; ++z) {
        uint8_t* p = &v[((size_t)x * NY + y) * NZ + z];
        if (z < 4) *p = rock;
        else if ((x < 16 || x >= 80) && z < 48) *p = brick;
        else if (z >= 48 && z < 56) *p = brick;
      }
  svxc_world* w = svxc_create(0.125);
  svxc_load_box(w, v, NX, NY, NZ, 0, 0, 0);
  free(v);
  svxc_set(w, "max_bodies", 2000);
  printf("designed: %d\n", svxc_bake(w));

  svxc_blast(w, 1.0, 1.0, 1.0, 0.8, 3e5);
  int added = 0, removed = 0, cracks = 0;
  for (int t = 0; t < 600; ++t) {
    svxc_tick(w);
    const int n = svxc_poll_events(w);
    for (int i = 0; i < n; ++i) {
      svxc_event e;
      svxc_event_at(w, i, &e);
      added += e.kind == SVXC_PIECE_ADDED;
      removed += e.kind == SVXC_PIECE_REMOVED;
      cracks += e.kind == SVXC_CRACK;
    }
    const int32_t* chunks;
    svxc_poll_changed_chunks(w, &chunks); /* (a renderer would re-mesh these) */
  }
  svxc_stats s;
  svxc_get_stats(w, &s);
  const int np = svxc_poll_pieces(w);
  printf("pieces: %d added, %d removed, %d now (%lld awake); %d cracks, %lld bonds broken\n", added, removed, np,
         (long long)s.awake, cracks, (long long)s.bonds_broken);
  for (int i = 0; i < np && i < 5; ++i) {
    svxc_piece p;
    svxc_piece_at(w, i, &p);
    int lo[3], dim[3];
    const uint8_t* shape = svxc_piece_voxels(w, p.id, lo, dim);
    printf("  piece %lld: %d voxels (%dx%dx%d box%s), %.0f kg at (%.2f %.2f %.2f)\n", (long long)p.id, p.voxels, dim[0], dim[1],
           dim[2], shape ? "" : ", no shape", p.mass, p.pos[0], p.pos[1], p.pos[2]);
  }
  size_t bytes = 0;
  svxc_save_delta(w, &bytes);
  printf("delta %zu bytes, state %016llx\n", bytes, (unsigned long long)svxc_state_hash(w));
  svxc_destroy(w);
  return 0;
}

// svx_city tests — the city's streets, highways, walkways and parking as structvox's road network
// (svx/city/roads.hpp; voxel_city svx/roads.js, svx/highwayLanes.js) against the reference (stage
// "svxroads", on worlds as the export makes them).
#include <doctest.h>

#include "records.hpp"
#include "svx/ids.hpp"
#include "svxroads_records.hpp"

using namespace svx::city;
using rec::Line;

TEST_CASE("city svx roads: lanes, ways on, signals, walks, corners, parking conform (stage svxroads)") {
  rec::Out out;
  for (const test::RoadWorld& rw : test::road_worlds()) {
    const std::shared_ptr<World> w = test::export_world(rw.preset, rw.size);
    const std::vector<test::RoadBox> boxes = test::road_boxes(*w);
    const RoadNetwork net(w, RoadNetworkOptions{1 << 16, 1 << 10, 1 << 20});
    // (ids of a road at the spawn, before anything is handed out and after)
    const CellIJ c = w->cell_at(0, 0);
    const std::string road = w->road_view(c.i, c.j)->segs[0].road->id;
    const uint64_t ids[4] = {1, static_cast<uint64_t>(id_of(road, "lane", 0, 0, 0, 0)), static_cast<uint64_t>(id_of(road, "walk", 0, 1)),
                             static_cast<uint64_t>(id_of(road, "lane", 0, 0, 1, 0))};
    out << (Line() << "world" << rw.key << boxes.size() << road);
    for (const uint64_t id : ids) out << test::road_probe(net, "before", id);
    for (const test::RoadBox& b : boxes) test::road_box_records(out, net, b);
    for (const uint64_t id : ids) out << test::road_probe(net, "after", id);
  }
  CHECK(rec::record("svxroads", out.text()) == rec::recorded_digest("svxroads"));
}

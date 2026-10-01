// svx_city tests — the road network (svx/city/roads.hpp) from several threads at once: one network
// over one world, keeping few road structures and highway edges' lanes made (dropped ones made
// again alike), four threads asking for boxes in orders of their own; a box's records (its lanes,
// ways on, signals, the lanes they lead to, walks, corners, parking) are those a lone thread
// writes, which conform to the reference (stage svxroads).
#include <doctest.h>

#include <string>
#include <thread>
#include <vector>

#include "records.hpp"
#include "svxroads_records.hpp"

using namespace svx::city;

TEST_CASE("city svx roads: four threads, any order, small caches - the same records") {
  const std::shared_ptr<World> w = test::export_world("angledInfiniteCity");
  std::vector<test::RoadBox> boxes{{"near", {-120, -120}, {120, 120}}};
  for (const test::RoadBox& b : test::road_boxes(*w))
    if (b.tag != "spawn") boxes.push_back(b);
  REQUIRE(boxes.size() >= 4);
  // (one thread alone, nothing dropped)
  std::vector<std::string> expect;
  {
    const RoadNetwork net(w, RoadNetworkOptions{1 << 16, 1 << 10, 1 << 20, 1 << 16});
    for (const test::RoadBox& b : boxes) {
      rec::Out out;
      test::road_box_records(out, net, b);
      expect.push_back(out.text());
    }
  }
  const RoadNetwork net(w, RoadNetworkOptions{128, 2, 1 << 20, 1 << 16});
  std::vector<std::vector<std::string>> got(4, std::vector<std::string>(boxes.size()));
  std::vector<std::thread> threads;
  for (size_t t = 0; t < 4; ++t)
    threads.emplace_back([&, t] {
      for (size_t k = 0; k < boxes.size(); ++k) {
        // (each from a box of its own, forwards or backwards)
        const size_t b = t % 2 == 0 ? (k + t) % boxes.size() : (boxes.size() * 2 - k - t) % boxes.size();
        rec::Out out;
        test::road_box_records(out, net, boxes[b]);
        got[t][b] = out.text();
      }
    });
  for (std::thread& th : threads) th.join();
  for (size_t t = 0; t < got.size(); ++t)
    for (size_t b = 0; b < boxes.size(); ++b) CHECK_MESSAGE(got[t][b] == expect[b], "thread ", t, ", box ", boxes[b].tag);
}

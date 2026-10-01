// svx_city tests — canonical building frames (voxel_city buildings/frame.js) against the
// reference (stage "frames" of tools/procgen_ref).
#include <doctest.h>

#include "buildings/frame.hpp"
#include "records.hpp"

using namespace svx::city;
using rec::Line;

namespace {

const char kCSides[4] = {'F', 'B', 'L', 'R'};
const char kWorldSides[4] = {'N', 'E', 'S', 'W'};

// (a side JS leaves undefined: '\0')
std::string side(char c) { return c ? std::string(1, c) : std::string("-"); }

Line frame_line(const char* tag, const Frame& f) {
  Line l;
  l << tag << f.R.x0 << f.R.y0 << f.R.x1 << f.R.y1 << f.front << f.U << f.V << f.turned << f.ou << f.ov;
  for (char s : kCSides) l << side(f.world_side(s));
  for (char s : kWorldSides) l << side(f.canon_side(s));
  return l;
}

void push(Line& l, const XY& p) { l << p[0] << p[1]; }

}  // namespace

TEST_CASE("city frames: building frames are the reference's (stage frames)") {
  rec::Samples r(13);
  auto big = [&](double n) { return std::floor((r() - 0.5) * n); };
  rec::Out out;
  {
    Line l;
    l << "cside";
    for (char s : kCSides) {
      const auto d = cside_dir(s);
      l << d[0] << d[1] << copp(s);
    }
    out << l;
  }
  for (int yaw = 0; yaw < 132; ++yaw)
    out << (Line() << "nf" << yaw << nominal_front(yaw) << nominal_front(yaw, (yaw * 7) % 132) << nominal_front(yaw, 1 + (yaw % 5)));
  for (int i = 0; i < 400; ++i) {
    const double x0 = big(4000);
    const double y0 = big(4000);
    const double w = std::floor(r() * 60);
    const double h = std::floor(r() * 60);
    const Rect R{x0, y0, x0 + w, y0 + h};
    const char front = kWorldSides[static_cast<int>(std::floor(r() * 4))];
    const Frame f(R, front);
    out << frame_line("f", f);
    Line l;
    l << "fm" << f.placement.px << f.placement.py << f.placement.q << f.placement.origin.x << f.placement.origin.y;
    for (int k = 0; k < 4; ++k) {
      const double u = big(80);
      const double v = big(80);
      push(l, f.to_world(u, v));
      push(l, f.from_world(R.x0 + u, R.y0 + v));
      push(l, f.dir_to_world(u, v));
    }
    Rect cr;
    cr.x0 = std::floor(r() * 10);
    cr.y0 = std::floor(r() * 10);
    cr.x1 = 10 + std::floor(r() * 30);
    cr.y1 = 10 + std::floor(r() * 30);
    const Rect wr = f.rect_to_world(cr);
    const Rect br = f.rect_from_world(wr);
    l << wr.x0 << wr.y0 << wr.x1 << wr.y1 << br.x0 << br.y0 << br.x1 << br.y1;
    out << l;
  }
  for (int i = 0; i < 400; ++i) {
    const int yaw = static_cast<int>(std::floor(r() * 132));
    const int yaw2 = r() < 0.3 ? static_cast<int>(std::floor(r() * 132)) : 0;
    const double ox = big(4000);
    const double oy = big(4000);
    PlacementOpts po;
    po.origin = {ox, oy, 0};
    po.yaw = yaw;
    po.yaw2 = yaw2;
    const Placement p(po);
    const double ou = big(40);
    const double ov = big(40);
    const double U = 8 + std::floor(r() * 60);
    const double V = 8 + std::floor(r() * 60);
    const Frame f = TurnedFrame(p, ou, ov, U, V, nominal_front(p.yaw, p.yaw2));
    out << frame_line("t", f);
    Line l;
    l << "tm";
    for (int k = 0; k < 4; ++k) {
      const double u = big(80);
      const double v = big(80);
      const XY w = f.to_world(u, v);
      push(l, w);
      push(l, f.from_world(w[0], w[1]));
      const double dx = big(9);
      const double dy = big(9);
      push(l, f.from_world(w[0] + dx, w[1] + dy));
      push(l, f.dir_to_world(u, v));
      const double fu = u + r() * 3;
      const double fv = v - r() * 3;
      const XY pw = f.point_to_world(fu, fv);
      push(l, pw);
      push(l, f.point_from_world(pw[0], pw[1]));
      push(l, f.point_from_world(pw[0] + 0.25, pw[1] - 7.5));
    }
    Rect cr;
    cr.x0 = std::floor(r() * 10);
    cr.y0 = std::floor(r() * 10);
    cr.x1 = 10 + std::floor(r() * 30);
    cr.y1 = 10 + std::floor(r() * 30);
    const Rect fr{cr.x0 + 0.5, cr.y0 - 0.25, cr.x1 - 0.75, cr.y1 + 0.5};
    const Rect wr = f.rect_to_world(cr);
    const Rect wf = f.rect_to_world(fr);
    const Rect br = f.rect_from_world(wr);
    l << wr.x0 << wr.y0 << wr.x1 << wr.y1 << wf.x0 << wf.y0 << wf.x1 << wf.y1 << br.x0 << br.y0 << br.x1 << br.y1;
    for (int k = 0; k < 3; ++k) {
      const double dx = big(100);
      const double dy = big(100);
      l << f.distance(cr, wr.x0 + dx, wr.y0 + dy);
    }
    out << l;
    const double sdu = big(10);
    const double sdv = big(10);
    const double sU = 5 + std::floor(r() * 20);
    const double sV = 5 + std::floor(r() * 20);
    const Frame s = f.shifted(sdu, sdv, sU, sV);
    out << frame_line("ts", s);
    const Turn t = *s.turn();
    Line lt;
    lt << "turn" << t.yaw;
    if (t.yaw2)
      lt << t.yaw2;
    else
      lt << rec::kUndef;
    lt << t.origin.x << t.origin.y << t.ou << t.ov;
    push(lt, s.to_world(1, 2));
    push(lt, s.from_world(ox, oy));
    out << lt;
    Turn turn = t;
    turn.U = s.U + 3;
    turn.V = s.V + 1;
    out << frame_line("tf", turned_frame(turn, s.U, s.V));
    out << frame_line("lf", lot_frame_of(turn, Rect{0, 0, 9, 9}, kWorldSides[i % 4]));
    const Frame ef = frame_of(i % 3 ? std::optional<Turn>(turn) : std::nullopt, 12, 20, kWorldSides[(i + 1) % 4], Rect{ox, oy, ox + 11, oy + 19});
    out << frame_line("ef", ef);
    Line le;
    le << "efm";
    push(le, ef.to_world(3, 4));
    push(le, ef.from_world(ox + 5, oy + 6));
    out << le;
  }
  out << frame_line("lp", lot_frame_of(std::nullopt, Rect{10, 20, 40, 35}, 'E'));
  CHECK(rec::record("frames", out.text()) == rec::recorded_digest("frames"));
}

TEST_CASE("city frames: a plain frame's canonical cells") {
  const Frame f(Rect{10, 20, 19, 25}, 'E');  // facing east: u runs north to south... along x = 19
  CHECK(f.U == 6);
  CHECK(f.V == 10);
  const XY w = f.to_world(0, 0);
  CHECK(w[0] == 19);
  CHECK(w[1] == 20);
  CHECK(f.world_side('F') == 'E');
  CHECK(f.canon_side('W') == 'B');
  CHECK(!f.turn());
}

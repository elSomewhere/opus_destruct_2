// svx_city tests — the generator's core (voxel_city core/*.js): the JS semantics layer, hashing,
// the seeded stream, noise, placements, oriented rects, 2D geometry, rects - against the
// reference (stage "core" of tools/procgen_ref).
#include <doctest.h>

#include "core/geom2d.hpp"
#include "core/hash.hpp"
#include "core/js.hpp"
#include "core/noise.hpp"
#include "core/obb.hpp"
#include "core/placement.hpp"
#include "core/rect.hpp"
#include "records.hpp"

using namespace svx::city;
using rec::Line;

TEST_CASE("city js: Math and Number semantics") {
  CHECK(js::round(2.5) == 3);
  CHECK(js::round(-2.5) == -2);
  CHECK(js::round(0.49999999999999994) == 0);
  CHECK(std::signbit(js::round(-0.3)));
  CHECK(js::round(4503599627370497.0) == 4503599627370497.0);
  CHECK(js::num(0.1 + 0.2) == "0.30000000000000004");
  CHECK(js::num(1e21) == "1e+21");
  CHECK(js::num(1e-7) == "1e-7");
  CHECK(js::num(123456789012345680000.0) == "123456789012345680000");
  CHECK(js::num(-0.0) == "0");
  CHECK(js::num(5e-324) == "5e-324");
  CHECK(js::to_int32(4294967296.0 + 5) == 5);
  CHECK(js::to_int32(-2147483649.0) == 2147483647);
  CHECK(js::imul(0x7fffffff, 0x7fffffff) == 1);
  CHECK(js::pow(2, 10) == 1024);
  CHECK(js::log2(8) == 3);
  CHECK(js::hypot(3, 4) == 5);
  std::vector<std::string> s = {"10", "9", "1"};
  js::sort_strings(s);
  CHECK(s == std::vector<std::string>{"1", "10", "9"});
}

TEST_CASE("city core: conforms to the reference (stage core)") {
  rec::Samples r(1);
  auto big = [&] { return std::floor((r() - 0.5) * 17179869184.0); };
  rec::Out out;
  for (int i = 0; i < 2000; ++i) {
    const double a = big(), b = (r() - 0.5) * 1e5, c = std::floor(r() * 1e6), d = r() * 3;
    out << (Line() << "h" << mix32(a) << hash32(a, b, c, d) << hash32(a) << hash_float(a, b) << hash32(c, d));
  }
  for (const std::string& str : {std::string(""), std::string("a"), std::string("building"), std::string("C12_-3/b4/l2/B"), std::string("tree"), std::string(40, 'x')})
    out << (Line() << "hs" << str << hash_string(str));
  for (int i = 0; i < 2000; ++i) {
    const double root = big(), n = (r() - 0.5) * 1e4, k = std::floor(r() * 100);
    out << (Line() << "ds" << derive_seed(root, "building", n) << derive_seed(root, n, k, "x") << derive_seed(root) << derive_seed(root, n * 1e6));
  }
  for (int i = 0; i < 300; ++i) {
    Rng g = Rng::from(big(), "rng", i);
    Line l;
    l << "rng";
    for (int k = 0; k < 8; ++k) l << g.next();
    l << g.int_(-5, 17) << g.float_(2, 9) << (g.chance(0.3) ? 1 : 0) << g.sign();
    const std::vector<double> five = {1, 2, 3, 4, 5};
    l << g.pick(five) << g.gauss(1, 2);
    l << g.weighted(std::vector<std::pair<double, double>>{{1, 0.5}, {2, 3}, {3, 1}});
    const std::vector<std::pair<double, double>> wv = {{2, 7}, {1, 8}};
    l << wv[g.weighted_index(wv, [](const auto& p) { return p.first; })].second;
    for (double v : g.shuffle(std::vector<double>{1, 2, 3, 4, 5, 6, 7})) l << v;
    Rng h = g.fork("child");
    l << h.next();
    l << h.next();
    out << l;
  }
  for (const double seed : {1337.0, 42.0, -7.0}) {
    SimplexNoise nz(seed);
    for (int i = 0; i < 1500; ++i) {
      const double x = (r() - 0.5) * 2000, y = (r() - 0.5) * 2000, z = (r() - 0.5) * 50, w = (r() - 0.5) * 300;
      out << (Line() << "n" << nz.n2(x, y) << nz.n3(x, y, z) << nz.n4(x, y, z, w) << nz.fbm2(x, y) << nz.fbm3(x / 7, y / 7, z)
                     << nz.fbm4(x, y, z, w, 5, 2.1, 0.45) << nz.ridged2(x, y) << nz.ridged3(x, y, z) << nz.ridged4(x, y, z, w)
                     << nz.nP(x, y, z, js::kNaN) << nz.fbmP(x, y, z, w) << nz.ridgedP(x, y, z, js::kNaN));
    }
    for (int i = 0; i < 200; ++i) {
      const double x = (r() - 0.5) * 1e10, y = (r() - 0.5) * 1e10;
      out << (Line() << "nf" << nz.n2(x, y) << nz.n3(x, y, x / 3) << nz.n4(x, y, y / 5, x / 7));
    }
  }
  for (const Yaw& y : yaws()) out << (Line() << "yaw" << y.c << y.s << y.r);
  for (const Yaw& p : pitches()) out << (Line() << "pitch" << p.c << p.s << p.r << p.n);
  for (int i = 0; i < 400; ++i) {
    const int yaw = static_cast<int>(std::floor(r() * 132));
    const int pitch = static_cast<int>(std::floor(r() * 13)) - 6;
    const int roll = static_cast<int>(std::floor(r() * 132));
    const double coin = r();
    const int yaw2 = coin < 0.5 ? 0 : static_cast<int>(std::floor(r() * 132));
    const RotMatrix rot = rotation_matrix(yaw, pitch, roll, yaw2);
    const QuatXYZW q = matrix_quat(rot.m, rot.d);
    const int other = static_cast<int>(std::floor(r() * 132));
    const Yaw yp = yaw_product(yaw, other);
    const double ndx = r() - 0.5;
    const double ndy = r() - 0.5;
    Line lr;
    lr << "rot" << yaw << pitch << roll << yaw2;
    lr.seq(rot.m.begin(), rot.m.end());
    lr << rot.d << q.x << q.y << q.z << q.w << yp.c << yp.s << yp.r << yaw_index(yp) << nearest_yaw(ndx, ndy);
    out << lr;
    PlacementOpts po;
    po.origin.x = std::fmod(big(), 4096);
    po.origin.y = std::fmod(big(), 4096);
    po.origin.z = std::floor(r() * 300);
    po.yaw = yaw;
    po.yaw2 = yaw2;
    po.pitch = i % 3 ? 0 : pitch;
    po.roll = i % 5 ? 0 : roll;
    po.extent = LocalBox{-3, 0, 0, 40, 25, 70};
    const Placement pl(po);
    Line l;
    l << "pl" << pl.yaw << pl.yaw2 << pl.q;
    if (pl.q >= 0)
      l << pl.px << pl.py;
    else
      l << rec::kUndef << rec::kUndef;
    for (int k = 0; k < 6; ++k) {
      const double x = std::fmod(big(), 4096), yy = std::fmod(big(), 4096), z = std::floor(r() * 300);
      const auto a = pl.to_local(x, yy, z);
      const auto b = pl.to_world(std::fmod(x, 50), std::fmod(yy, 50), std::fmod(z, 50));
      const auto c = pl.to_local_xy(x, yy);
      const auto d = pl.to_world_xy(std::fmod(x, 30), std::fmod(yy, 30));
      const auto e = pl.dir_to_world_xy(3, -2);
      l.seq(a.begin(), a.end()).seq(b.begin(), b.end()).seq(c.begin(), c.end()).seq(d.begin(), d.end()).seq(e.begin(), e.end());
    }
    const Box3 bb = *pl.world_aabb();
    l << bb.x0 << bb.y0 << bb.z0 << bb.x1 << bb.y1 << bb.z1;
    out << l;
    const double cpx = std::fmod(big(), 999), cpy = std::fmod(big(), 999);
    PlacementOpts co;
    co.origin.z = 5;
    const Placement c = Placement::cardinal(cpx, cpy, i % 4, co);
    Line lc;
    lc << "plc" << c.px << c.py << c.q;
    const auto c1 = c.to_local_xy(3.5, -2);
    const auto c2 = c.to_world_xy(1.25, 7);
    const auto c3 = c.to_world(2, 3, 4);
    lc.seq(c1.begin(), c1.end()).seq(c2.begin(), c2.end()).seq(c3.begin(), c3.end());
    out << lc;
    PlacementOpts fo;
    fo.origin.x = std::fmod(big(), 999);
    fo.origin.y = std::fmod(big(), 999);
    fo.yaw = yaw;
    const Placement fl(fo);
    const Rect lrct{-5, 2, 20, 31};
    const Rect bnd = obb_bounds(fl, lrct);
    const XY lp = local_point_to_world(fl, 2.5, 7);
    const XY wp = world_point_to_local(fl, fl.origin.x + 11, fl.origin.y - 4);
    out << (Line() << "obb" << bnd.x0 << bnd.y0 << bnd.x1 << bnd.y1 << obb_distance(fl, lrct, fl.origin.x + 40, fl.origin.y - 3)
                   << (obb_contains(fl, lrct, fl.origin.x + 3, fl.origin.y + 9, 1) ? 1 : 0) << lp[0] << lp[1] << wp[0] << wp[1]);
    std::vector<XY> poly;
    for (const XY& cr : obb_corners(fl, Rect{0, 0, 120, 90})) {
      const double dx = r() * 4;
      const double dy = r() * 4;
      poly.push_back({cr[0] + dx, cr[1] + dy});
    }
    PlacementOpts fo2;
    fo2.origin = fl.origin;
    fo2.yaw = static_cast<int>(std::floor(r() * 132));
    FitOpts fopt;
    fopt.depth_min = 10, fopt.depth_max = 60, fopt.min_width = 8, fopt.max_width = 40, fopt.front_slack = 3;
    const auto fit = fit_local_rect(Placement(fo2), poly, fopt);
    const double nx = r() - 0.5, ny = r() - 0.5, cc = (r() - 0.5) * 100;
    const auto clip = clip_half_plane(poly, nx, ny, cc);
    Line lf;
    lf << "fit";
    if (fit)
      lf << std::vector<double>{fit->x0, fit->y0, fit->x1, fit->y1};
    else
      lf << rec::kUndef;
    for (const XY& p : clip) lf << p[0] << p[1];
    out << lf;
  }
  for (int i = 0; i < 500; ++i) {
    const double a = std::fmod(big(), 500), b = std::fmod(big(), 500), c = std::fmod(big(), 500), d = std::fmod(big(), 500),
                 e = std::fmod(big(), 500), g = std::fmod(big(), 500);
    const SegProj p = project_to_segment(a, b, c, d, e, g);
    out << (Line() << "seg" << p.t << p.raw_t << p.dist << p.side << p.along << p.len << p.cx << p.cy);
  }
  for (int i = 0; i < 40; ++i) {
    std::vector<PPoint> pts;
    for (int k = 0; k < 2 + (i % 5); ++k) {
      PPoint q;
      q.x = std::fmod(big(), 900);
      q.y = std::fmod(big(), 900);
      if (i % 2) q.z = r() * 30;
      pts.push_back(q);
    }
    const auto cr = catmull_rom(pts, 6 + (i % 3), i % 4 == 3);
    const auto L = polyline_lengths(cr);
    const PolyAt at = polyline_at(cr, L, L.back() * r());
    Line l;
    l << "cr";
    for (const PPoint& q : cr) {
      l << q.x << q.y;
      if (q.has_z())
        l << q.z;
      else
        l << rec::kUndef;
    }
    l << L.back() << at.x << at.y << at.tx << at.ty;
    if (at.z == at.z)
      l << at.z;
    else
      l << rec::kUndef;
    out << l;
    const Rect b = points_bounds(pts, 2.5);
    out << (Line() << "pb" << b.x0 << b.y0 << b.x1 << b.y1);
  }
  SpatialGrid<int> grid(64);
  for (int i = 0; i < 300; ++i) {
    const double x0 = std::fmod(big(), 2000), y0 = std::fmod(big(), 2000);
    const double w = std::floor(r() * 150), h = std::floor(r() * 150);
    grid.insert(i, Rect{x0, y0, x0 + w, y0 + h});
  }
  for (int i = 0; i < 200; ++i) {
    const double x0 = std::fmod(big(), 2000), y0 = std::fmod(big(), 2000);
    const double w = std::floor(r() * 400), h = std::floor(r() * 400);
    Line l;
    l << "sg";
    for (int v : grid.query(Rect{x0, y0, x0 + w, y0 + h})) l << v;
    l << "|";
    for (int v : grid.query_point(x0, y0)) l << v;
    out << l;
  }
  for (int i = 0; i < 300; ++i) {
    Rect a;
    a.x0 = std::fmod(big(), 100);
    a.y0 = std::fmod(big(), 100);
    a.x1 = a.x0 + std::floor(r() * 60);
    a.y1 = a.y0 + std::floor(r() * 60);
    Rect b;
    b.x0 = a.x0 + std::floor((r() - 0.3) * 50);
    b.y0 = a.y0 + std::floor((r() - 0.3) * 50);
    b.x1 = b.x0 + std::floor(r() * 60);
    b.y1 = b.y0 + std::floor(r() * 60);
    const auto it = r_intersect(a, b);
    const auto w = r_shared_wall(a, Rect{a.x1 + 2, a.y0 + 3, a.x1 + 9, a.y1 + 2}, 1);
    Line l;
    l << "r" << r_area(a);
    if (it)
      l << std::vector<double>{it->x0, it->y0, it->x1, it->y1};
    else
      l << rec::kUndef;
    std::vector<double> sub;
    for (const Rect& q : r_subtract(a, b)) sub.insert(sub.end(), {q.x0, q.y0, q.x1, q.y1});
    l << sub;
    if (w)
      l << ("[" + std::string(1, w->orient) + "," + js::num(w->x0) + "," + js::num(w->x1) + "," + js::num(w->t0) + "," + js::num(w->t1) + "," + std::string(1, w->side_of_a) + "]");
    else
      l << rec::kUndef;
    l << r_key(r_edge_strip(a, "SENW"[i % 4], 2)) << r_key(r_outer_strip(a, "NWSE"[i % 4], 3));
    out << l;
  }
  const std::string got = rec::record("core", out.text());
  CHECK(got == rec::recorded_digest("core"));
}

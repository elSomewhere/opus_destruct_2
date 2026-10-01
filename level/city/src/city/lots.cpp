// svx_city — voxel_city city/lots.js.
#include "city/lots.hpp"

#include <initializer_list>
#include <utility>

#include "core/math.hpp"

namespace svx::city {

namespace {

// (JS truthiness of a string field: null and "" are falsy)
bool truthy(const std::optional<std::string>& s) { return s && !s->empty(); }

// block.sides[side] (a side without a road is NO_SIDE: cls null, as JS's undefined reads)
const RoadSide& side_of(const BlockSides& s, char side) {
  switch (side) {
    case 'N': return s.N;
    case 'S': return s.S;
    case 'W': return s.W;
    default: return s.E;
  }
}

bool is_street(const RoadSide& s) { return truthy(s.cls) && *s.cls != "alley"; }

std::vector<Frontage> frontages_of(const Block& block, const Rect& rect) {
  const Rect& p = block.prop;
  std::vector<Frontage> out;
  if (rect.y0 == p.y0 && is_street(block.sides.N)) out.push_back({'N', *block.sides.N.cls});
  if (rect.y1 == p.y1 && is_street(block.sides.S)) out.push_back({'S', *block.sides.S.cls});
  if (rect.x0 == p.x0 && is_street(block.sides.W)) out.push_back({'W', *block.sides.W.cls});
  if (rect.x1 == p.x1 && is_street(block.sides.E)) out.push_back({'E', *block.sides.E.cls});
  return out;
}

char alley_of(const Block& block, const Rect& rect) {
  const Rect& p = block.prop;
  for (const char side : {'N', 'S', 'W', 'E'}) {
    const RoadSide& s = side_of(block.sides, side);
    if (!s.cls || *s.cls != "alley") continue;
    if (side == 'N' && rect.y0 == p.y0) return side;
    if (side == 'S' && rect.y1 == p.y1) return side;
    if (side == 'W' && rect.x0 == p.x0) return side;
    if (side == 'E' && rect.x1 == p.x1) return side;
  }
  return 0;
}

using Piece = std::array<double, 2>;

// Split [a0, a1] (inclusive) into pieces drawn from [w_min, w_max] voxels; the first and last
// pieces (corner lots) are widened by `corner_boost`.
std::vector<Piece> split_widths(double a0, double a1, double w_min, double w_max, Rng& rng, double corner_boost = 1.35) {
  const double len = a1 - a0 + 1;
  if (len < w_min * 2) return {{a0, a1}};
  std::vector<Piece> out;
  double a = a0;
  double rem = len;
  double k = 0;
  while (rem > 0) {
    double w = js::round((rng.float_(w_min, w_max) * (k == 0 ? corner_boost : 1)) / 4) * 4;
    if (rem - w < w_min * corner_boost) w = rem <= w_max * corner_boost * 1.2 ? rem : js::round(rem / 2 / 4) * 4;
    out.push_back({a, a + w - 1});
    a += w;
    rem -= w;
    k += 1;
  }
  return out;
}

// makeLot(block, rect, rng, extra): the extras are set by the caller after (an extra's `front`
// replaces the computed one, as JS's spread does). (The reference passes rng and draws nothing.)
Lot make_lot(const Block& block, const Rect& rect) {
  Lot lot;
  lot.rect = rect;
  lot.block = block.id;
  lot.cell = block.cell;
  lot.district = block.district;
  lot.frontages = frontages_of(block, rect);
  lot.alley = alley_of(block, rect);
  const char front = main_frontage(rect, lot.frontages);
  lot.front = front ? front : (lot.alley ? opposite(lot.alley) : 'S');
  lot.corner = lot.frontages.size() >= 2;
  return lot;
}

// Two back-to-back rows (or one row) of lots along the long axis.
std::vector<Lot> row_lots(const Block& block, Rng& rng, const std::array<double, 2>& w_range, double two_row_min) {
  const Rect& p = block.prop;
  const double W = rw(p);
  const double H = rh(p);
  const bool long_x = W >= H;
  const double short_len = long_x ? H : W;
  bool alley_side = false;
  for (const char s : {'N', 'S', 'W', 'E'}) {
    const RoadSide& rs = side_of(block.sides, s);
    if (rs.cls && *rs.cls == "alley") {
      alley_side = true;
      break;
    }
  }
  std::vector<Lot> lots;
  const double w_min = vx(w_range[0]);
  const double w_max = vx(w_range[1]);
  std::vector<Rect> rows;
  const bool two_rows = !alley_side && short_len >= vx(two_row_min);
  if (two_rows) {
    const double mid = long_x ? js::round((p.y0 + p.y1) / 2) : js::round((p.x0 + p.x1) / 2);
    if (long_x)
      rows = {Rect{p.x0, p.y0, p.x1, mid}, Rect{p.x0, mid + 1, p.x1, p.y1}};
    else
      rows = {Rect{p.x0, p.y0, mid, p.y1}, Rect{mid + 1, p.y0, p.x1, p.y1}};
  } else {
    rows = {p};
  }
  for (const Rect& row : rows) {
    const std::vector<Piece> pieces = long_x ? split_widths(row.x0, row.x1, w_min, w_max, rng) : split_widths(row.y0, row.y1, w_min, w_max, rng);
    for (const Piece& q : pieces) {
      const Rect rect = long_x ? Rect{q[0], row.y0, q[1], row.y1} : Rect{row.x0, q[0], row.x1, q[1]};
      lots.push_back(make_lot(block, rect));
    }
  }
  return lots;
}

std::vector<Lot> downtown_lots(const Block& block, Rng& rng) {
  const Rect& p = block.prop;
  const double W = rw(p);
  const double H = rh(p);
  const bool long_x = W >= H;
  const double r = rng.next();
  if (r < 0.34 || js::max(W, H) < vx(60)) {
    Lot lot = make_lot(block, p);
    lot.whole = true;
    return {lot};
  }
  if (r < 0.78) {
    // halves across the long axis
    const double mid = long_x ? js::round((p.x0 + p.x1) / 2 + rng.float_(-0.12, 0.12) * W) : js::round((p.y0 + p.y1) / 2 + rng.float_(-0.12, 0.12) * H);
    Rect a = p;
    Rect b = p;
    if (long_x) {
      a.x1 = mid;
      b.x0 = mid + 1;
    } else {
      a.y1 = mid;
      b.y0 = mid + 1;
    }
    return {make_lot(block, a), make_lot(block, b)};
  }
  const double mx = js::round((p.x0 + p.x1) / 2);
  const double my = js::round((p.y0 + p.y1) / 2);
  return {make_lot(block, Rect{p.x0, p.y0, mx, my}), make_lot(block, Rect{mx + 1, p.y0, p.x1, my}), make_lot(block, Rect{p.x0, my + 1, mx, p.y1}),
          make_lot(block, Rect{mx + 1, my + 1, p.x1, p.y1})};
}

std::vector<Lot> industrial_lots(const Block& block, Rng& rng, const std::array<double, 2>& w_range) {
  const Rect& p = block.prop;
  const double W = rw(p);
  const double H = rh(p);
  const bool long_x = W >= H;
  const std::vector<Piece> pieces =
      long_x ? split_widths(p.x0, p.x1, vx(w_range[0]), vx(w_range[1]), rng, 1) : split_widths(p.y0, p.y1, vx(w_range[0]), vx(w_range[1]), rng, 1);
  const double short_len = long_x ? H : W;
  std::vector<Lot> lots;
  for (const Piece& q : pieces) {
    const double a0 = q[0];
    const double a1 = q[1];
    if (short_len > vx(150)) {
      const double mid = long_x ? js::round((p.y0 + p.y1) / 2) : js::round((p.x0 + p.x1) / 2);
      const Rect halves[2] = {long_x ? Rect{a0, p.y0, a1, mid} : Rect{p.x0, a0, mid, a1}, long_x ? Rect{a0, mid + 1, a1, p.y1} : Rect{mid + 1, a0, p.x1, a1}};
      for (const Rect& r : halves) lots.push_back(make_lot(block, r));
    } else {
      const Rect rect = long_x ? Rect{a0, p.y0, a1, p.y1} : Rect{p.x0, a0, p.x1, a1};
      lots.push_back(make_lot(block, rect));
    }
  }
  return lots;
}

// Countryside: farms set back behind the (wobbly) rural road on the block's road sides: a
// farmhouse plot with a barn plot beside it (the dressing adds silos, bales and fences); the rest
// of the block stays fields and nature.
std::vector<Lot> farmstead_lots(const Block& block, Rng& rng) {
  const Rect& p = block.prop;
  std::vector<Lot> out;
  const double setback = vx(30);
  const double margin = vx(60);
  auto clash = [&](const Rect& r) {
    for (const Lot& l : out)
      if (l.rect.x0 <= r.x1 + 16 && r.x0 <= l.rect.x1 + 16 && l.rect.y0 <= r.y1 + 16 && r.y0 <= l.rect.y1 + 16) return true;
    return false;
  };
  auto inside = [&](const Rect& r) { return r.x0 >= p.x0 && r.y0 >= p.y0 && r.x1 <= p.x1 && r.y1 <= p.y1; };
  for (const char side : {'N', 'S', 'W', 'E'}) {
    if (!truthy(side_of(block.sides, side).cls)) continue;
    const bool along_x = side == 'N' || side == 'S';
    const double a0 = (along_x ? p.x0 : p.y0) + margin;
    const double a1 = (along_x ? p.x1 : p.y1) - margin;
    for (double a = a0 + vx(rng.float_(0, 160)); a < a1; a += vx(rng.float_(260, 560))) {
      if (!rng.chance(0.62)) continue;
      const double depth = vx(rng.float_(34, 44));
      const double hw = vx(rng.float_(24, 30));
      const double bw = vx(rng.float_(24, 32));
      if (a + hw + vx(4) + bw > a1) break;
      auto rect_at = [&](double s0, double w) -> Rect {
        if (side == 'N') return {s0, p.y0 + setback, s0 + w - 1, p.y0 + setback + depth - 1};
        if (side == 'S') return {s0, p.y1 - setback - depth + 1, s0 + w - 1, p.y1 - setback};
        if (side == 'W') return {p.x0 + setback, s0, p.x0 + setback + depth - 1, s0 + w - 1};
        return {p.x1 - setback - depth + 1, s0, p.x1 - setback, s0 + w - 1};
      };
      const Rect house = rect_at(a, hw);
      const Rect barn = rect_at(a + hw + vx(4), bw);
      if (!inside(house) || !inside(barn) || clash(house) || clash(barn)) continue;
      const std::string farm = js::cat("f", static_cast<double>(out.size()));
      Lot h = make_lot(block, house);
      h.front = side;
      h.farmstead = true;
      h.farm_role = "house";
      h.farm = farm;
      out.push_back(std::move(h));
      Lot b = make_lot(block, barn);
      b.front = side;
      b.farmstead = true;
      b.farm_role = "barn";
      b.farm = farm;
      out.push_back(std::move(b));
    }
  }
  return out;
}

// Village: house plots strung along the block's road sides, set back enough to clear the gently
// bending main street; the cell plan thins them out towards the village edge. The block's
// interior stays fields and meadow.
std::vector<Lot> village_lots(const Block& block, Rng& rng) {
  const Rect& p = block.prop;
  std::vector<Lot> out;
  for (const char side : {'N', 'S', 'W', 'E'}) {
    const RoadSide& s = side_of(block.sides, side);
    if (!truthy(s.cls)) continue;
    // the village's own streets are straight; the country roads bend a little
    const double setback = truthy(s.id) ? vx(1) : vx(7);
    const bool along_x = side == 'N' || side == 'S';
    const double a0 = (along_x ? p.x0 : p.y0) + vx(8);
    const double a1 = (along_x ? p.x1 : p.y1) - vx(8);
    for (double a = a0 + vx(rng.float_(0, 6)); a < a1;) {
      const double width = vx(rng.float_(14, 24));
      const double depth = vx(rng.float_(28, 40));
      if (a + width > a1) break;
      if (rng.chance(0.93)) {
        Rect rect;
        if (side == 'N')
          rect = {a, p.y0 + setback, a + width - 1, p.y0 + setback + depth - 1};
        else if (side == 'S')
          rect = {a, p.y1 - setback - depth + 1, a + width - 1, p.y1 - setback};
        else if (side == 'W')
          rect = {p.x0 + setback, a, p.x0 + setback + depth - 1, a + width - 1};
        else
          rect = {p.x1 - setback - depth + 1, a, p.x1 - setback, a + width - 1};
        bool clash = false;
        for (const Lot& l : out)
          if (l.rect.x0 <= rect.x1 + 8 && rect.x0 <= l.rect.x1 + 8 && l.rect.y0 <= rect.y1 + 8 && rect.y0 <= l.rect.y1 + 8) {
            clash = true;
            break;
          }
        if (!clash && rect.x0 >= p.x0 && rect.y0 >= p.y0 && rect.x1 <= p.x1 && rect.y1 <= p.y1) {
          Lot lot = make_lot(block, rect);
          lot.front = side;
          lot.village = true;
          out.push_back(std::move(lot));
        }
      }
      a += width + vx(rng.float_(1, 5));
    }
  }
  return out;
}

std::vector<Lot> named(std::vector<Lot> lots, const std::string& block_id) {
  for (size_t k = 0; k < lots.size(); ++k) lots[k].id = js::cat(block_id, "/l", static_cast<double>(k));
  return lots;
}

}  // namespace

char main_frontage(const Rect& rect, const std::vector<Frontage>& frontages) {
  // (rank[f.cls] ?? 0)
  auto rank = [](const std::string& cls) -> double {
    if (cls == "arterial") return 5;
    if (cls == "collector") return 4;
    if (cls == "local" || cls == "village") return 3;
    if (cls == "pedestrian") return 2;
    if (cls == "rural") return 1;
    if (cls == "lane") return 0.5;
    return 0;
  };
  char front = 0;
  double best_score = -1;
  for (const Frontage& f : frontages) {
    const double len = f.side == 'N' || f.side == 'S' ? rw(rect) : rh(rect);
    const double score = len * (1 + 0.15 * rank(f.cls));
    if (score > best_score) {
      best_score = score;
      front = f.side;
    }
  }
  return front;
}

std::vector<Lot> row_lots_of(const Block& block, Rng& rng, const std::array<double, 2>& width) {
  return named(row_lots(block, rng, width, 40), block.id);
}

std::vector<Lot> micro_lots(const Block& block, Rng& rng) {
  const Rect& p = block.prop;
  const double m = vx(9);
  const Rect inner{p.x0 + m, p.y0 + m, p.x1 - m, p.y1 - m};
  const double W = inner.x1 - inner.x0;
  const double H = inner.y1 - inner.y0;
  if (W < vx(40) || H < vx(30)) return {};
  const bool along_x = W >= H;
  const double L = along_x ? W : H;
  const double D = along_x ? H : W;
  const double slab_d = vx(15);
  std::vector<Lot> out;
  double row = 0;
  auto front_of = [&]() -> char { return along_x ? ((js::to_int32(row) & 1) ? 'N' : 'S') : (js::to_int32(row) & 1) ? 'W' : 'E'; };
  for (double b = rng.int_(0, vx(6)); b + slab_d <= D; b += slab_d + vx(rng.float_(24, 36))) {
    double a = rng.int_(0, vx(10));
    while (a < L) {
      const bool tower = rng.chance(0.2) && D - b >= vx(26);
      const double len = tower ? vx(26) : vx(rng.float_(48, 110));
      const double dep = tower ? vx(26) : slab_d;
      if (a + len > L) {
        if (L - a >= vx(40) && !tower) {
          const Rect r2 = along_x ? Rect{inner.x0 + a, inner.y0 + b, inner.x1, inner.y0 + b + dep - 1} : Rect{inner.x0 + b, inner.y0 + a, inner.x0 + b + dep - 1, inner.y1};
          Lot lot = make_lot(block, r2);
          lot.front = front_of();
          lot.micro = true;
          lot.arch = "panelSlab";
          out.push_back(std::move(lot));
        }
        break;
      }
      const Rect rect = along_x ? Rect{inner.x0 + a, inner.y0 + b, inner.x0 + a + len - 1, inner.y0 + b + dep - 1}
                                : Rect{inner.x0 + b, inner.y0 + a, inner.x0 + b + dep - 1, inner.y0 + a + len - 1};
      if (rect.x1 <= inner.x1 && rect.y1 <= inner.y1) {
        Lot lot = make_lot(block, rect);
        lot.front = front_of();
        lot.micro = true;
        lot.arch = tower ? "panelTower" : "panelSlab";
        out.push_back(std::move(lot));
      }
      a += len + vx(rng.float_(14, 26));
    }
    row += 1;
  }
  return out;
}

Lot free_lot(const Block& block, const Rect& rect, char front) {
  Lot lot = make_lot(block, rect);
  lot.front = front;
  return lot;
}

Lot whole_block_lot(const Block& block) {
  Lot lot = make_lot(block, block.prop);
  lot.whole = true;
  lot.id = js::cat(block.id, "/l0");
  return lot;
}

std::vector<Lot> plan_block_lots(const Block& block, const District& district, Rng& rng) {
  const std::string& mode = district.lots.mode;
  std::vector<Lot> lots;
  if (mode == "downtown")
    lots = downtown_lots(block, rng);
  else if (mode == "perimeter")
    lots = row_lots(block, rng, district.lots.width, 40);
  else if (mode == "suburban")
    lots = row_lots(block, rng, district.lots.width, 50);
  else if (mode == "industrial")
    lots = industrial_lots(block, rng, district.lots.width);
  else if (mode == "rural")
    lots = farmstead_lots(block, rng);
  else if (mode == "village")
    lots = village_lots(block, rng);
  return named(std::move(lots), block.id);
}

}  // namespace svx::city

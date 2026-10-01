// svx_city tests — record formats shared by the prefab stages (prefabs, civicrules): optional
// fields as the reference's f() prints them ("-" for undefined) and a furniture prefab's options.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "buildings/interior/prefabs.hpp"
#include "core/js.hpp"

namespace svx::city::irec {

inline std::string fo(const std::optional<double>& v) { return v ? js::num(*v) : "-"; }
inline std::string fo(const std::optional<uint16_t>& v) { return v ? js::num(*v) : "-"; }
inline std::string fo(const std::optional<bool>& v) { return v ? (*v ? "1" : "0") : "-"; }

// A prefab's options as the stages print them: w, d, monitor, hood, upper, screen, metal, steel,
// double, band, piano, goods (a material or a list), h, top, curtain, seat, frame.
inline std::string prefab_opts(const PrefabOpts& o) {
  std::string goods = "-";
  if (o.goods)
    goods = js::num(*o.goods);
  else if (o.goods_list) {
    goods = "[";
    for (size_t k = 0; k < o.goods_list->size(); ++k) goods += js::cat(k ? "," : "", (*o.goods_list)[k]);
    goods += "]";
  }
  return js::cat(fo(o.w), ",", fo(o.d), ",", fo(o.monitor), ",", fo(o.hood), ",", fo(o.upper), ",", fo(o.screen), ",", fo(o.metal), ",", fo(o.steel), ",",
                 fo(o.double_), ",", fo(o.band), ",", fo(o.piano), ",", goods, ",", fo(o.h), ",", fo(o.top), ",", fo(o.curtain), ",", fo(o.seat), ",",
                 fo(o.frame));
}

}  // namespace svx::city::irec

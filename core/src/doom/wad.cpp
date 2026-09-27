#include "svx/doom/wad.hpp"

#include <cstdio>
#include <cstring>

namespace svx::doom {

namespace {

i16 rd16(const u8* p) { return static_cast<i16>(u16(p[0]) | (u16(p[1]) << 8)); }
u16 ru16(const u8* p) { return static_cast<u16>(u16(p[0]) | (u16(p[1]) << 8)); }
i32 rd32(const u8* p) { return static_cast<i32>(u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24)); }

std::array<char, 9> name8(const u8* p) {
  std::array<char, 9> s{};
  for (int i = 0; i < 8; ++i) s[i] = static_cast<char>(p[i]);
  s[8] = 0;
  return s;
}

bool is_map_marker(const std::string& n) {
  if (n.size() == 4 && n[0] == 'E' && n[2] == 'M' && n[1] >= '0' && n[1] <= '9' && n[3] >= '0' && n[3] <= '9')
    return true;
  if (n.size() == 5 && n.compare(0, 3, "MAP") == 0 && n[3] >= '0' && n[3] <= '9' && n[4] >= '0' && n[4] <= '9')
    return true;
  return false;
}

// Namespace marker lumps (F_START, F1_END, PP_START, ...).
bool is_namespace_marker(const std::string& key) {
  auto ends_with = [&](const char* s) {
    const size_t n = std::strlen(s);
    return key.size() >= n && key.compare(key.size() - n, n, s) == 0;
  };
  return ends_with("_START") || ends_with("_END");
}

}  // namespace

std::string lump_key(std::string_view name) {
  std::string k;
  for (size_t i = 0; i < name.size() && i < 8 && name[i] != '\0'; ++i) {
    const char c = name[i];
    k.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
  }
  return k;
}

bool Sector::sky() const { return std::strncmp(ceil_pic.data(), "F_SKY", 5) == 0; }

bool Wad::load(const std::string& path, std::string* err) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (err) *err = "cannot open " + path;
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<u8> bytes(sz > 0 ? static_cast<size_t>(sz) : 0);
  const size_t got = bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  if (sz < 12 || got != bytes.size()) {
    data_.clear();
    lumps_.clear();
    if (err) *err = "short read";
    return false;
  }
  return load_memory(std::move(bytes), err);
}

bool Wad::load_memory(std::vector<u8> bytes, std::string* err) {
  data_ = std::move(bytes);
  lumps_.clear();
  auto fail = [&](const char* msg) {
    data_.clear();
    if (err) *err = msg;
    return false;
  };
  if (data_.size() < 12) return fail("short read");
  if (std::memcmp(data_.data(), "IWAD", 4) != 0 && std::memcmp(data_.data(), "PWAD", 4) != 0) return fail("not a WAD");
  const i32 n = rd32(&data_[4]);
  const i32 dir = rd32(&data_[8]);
  // 64-bit math: size_t is 32-bit on WASM and n * 16 could wrap.
  if (n < 0 || dir < 0 || u64(dir) + u64(n) * 16 > u64(data_.size())) return fail("bad directory");
  lumps_.resize(n);
  for (i32 i = 0; i < n; ++i) {
    const u8* e = &data_[size_t(dir) + size_t(i) * 16];
    Lump& l = lumps_[i];
    l.pos = rd32(e);
    l.size = rd32(e + 4);
    char nm[9] = {};
    std::memcpy(nm, e + 8, 8);
    l.name = nm;
    l.key = lump_key(l.name);
  }
  return true;
}

const std::string& Wad::lump_name(i32 i) const {
  static const std::string kNone;
  return i >= 0 && i < lump_count() ? lumps_[i].name : kNone;
}

bool Wad::lump_in_bounds(i32 i) const {
  if (i < 0 || i >= lump_count()) return false;
  const Lump& l = lumps_[i];
  return l.pos >= 0 && l.size >= 0 && u64(l.pos) + u64(l.size) <= u64(data_.size());
}

std::span<const u8> Wad::lump_data(i32 i) const {
  if (!lump_in_bounds(i) || lumps_[i].size == 0) return {};
  return std::span<const u8>(data_.data() + lumps_[i].pos, size_t(lumps_[i].size));
}

std::vector<i32> Wad::lumps_between(std::string_view start, std::string_view end, std::string_view start2,
                                    std::string_view end2) const {
  const std::string s1 = lump_key(start), e1 = lump_key(end), s2 = lump_key(start2), e2 = lump_key(end2);
  auto is = [](const std::string& key, const std::string& marker) { return !marker.empty() && key == marker; };
  std::vector<i32> out;
  bool inside = false;
  for (i32 i = 0; i < lump_count(); ++i) {
    const std::string& k = lumps_[i].key;
    if (is(k, s1) || is(k, s2)) {
      inside = true;
    } else if (is(k, e1) || is(k, e2)) {
      inside = false;
    } else if (inside && !is_namespace_marker(k)) {
      out.push_back(i);
    }
  }
  return out;
}

std::vector<i32> Wad::namespace_lumps(LumpNamespace ns) const {
  switch (ns) {
    case LumpNamespace::Flats: return lumps_between("F_START", "F_END", "FF_START", "FF_END");
    case LumpNamespace::Patches: return lumps_between("P_START", "P_END", "PP_START", "PP_END");
    case LumpNamespace::Sprites: return lumps_between("S_START", "S_END", "SS_START", "SS_END");
    case LumpNamespace::Global: break;
  }
  std::vector<i32> out;
  for (i32 i = 0; i < lump_count(); ++i)
    if (!is_namespace_marker(lumps_[i].key)) out.push_back(i);
  return out;
}

i32 Wad::find_lump(std::string_view name, LumpNamespace ns) const {
  const std::string key = lump_key(name);
  if (key.empty()) return -1;
  if (ns == LumpNamespace::Global) {
    for (i32 i = lump_count() - 1; i >= 0; --i)
      if (lumps_[i].key == key) return i;
    return -1;
  }
  const std::vector<i32> in_ns = namespace_lumps(ns);
  for (size_t j = in_ns.size(); j-- > 0;)
    if (lumps_[in_ns[j]].key == key) return in_ns[j];
  return -1;
}

std::span<const u8> Wad::lump_by_name(std::string_view name, LumpNamespace ns) const {
  return lump_data(find_lump(name, ns));
}

std::vector<std::string> Wad::map_names() const {
  std::vector<std::string> out;
  for (size_t i = 0; i + 1 < lumps_.size(); ++i)
    if (is_map_marker(lumps_[i].name) && lumps_[i + 1].name == "THINGS") out.push_back(lumps_[i].name);
  return out;
}

bool Wad::read_map(const std::string& name, Map* out, std::string* err) const {
  size_t idx = lumps_.size();
  for (size_t i = 0; i < lumps_.size(); ++i)
    if (lumps_[i].name == name) {
      idx = i;
      break;
    }
  if (idx == lumps_.size()) {
    if (err) *err = "map not found: " + name;
    return false;
  }
  auto find = [&](const char* lump) -> const Lump* {
    for (size_t i = idx + 1; i < lumps_.size() && i < idx + 12; ++i) {
      if (lumps_[i].name == lump) return &lumps_[i];
      if (is_map_marker(lumps_[i].name)) break;
    }
    return nullptr;
  };
  if (find("BEHAVIOR")) {
    if (err) *err = "Hexen-format maps are not supported";
    return false;
  }
  const Lump* lv = find("VERTEXES");
  const Lump* ll = find("LINEDEFS");
  const Lump* ls = find("SIDEDEFS");
  const Lump* lsec = find("SECTORS");
  if (!lv || !ll || !ls || !lsec) {
    if (err) *err = "missing map lumps";
    return false;
  }
  for (const Lump* l : {lv, ll, ls, lsec})
    if (l->pos < 0 || l->size < 0 || u64(l->pos) + u64(l->size) > u64(data_.size())) {
      if (err) *err = "map lump outside the file: " + l->name;
      return false;
    }
  Map m;
  m.name = name;
  if (const Lump* lt = find("THINGS"); lt && lt->pos >= 0 && size_t(lt->pos) + size_t(lt->size) <= data_.size()) {
    for (i32 o = 0; o + 10 <= lt->size; o += 10) {
      const u8* p = &data_[size_t(lt->pos) + o];
      m.things.push_back({rd16(p), rd16(p + 2), ru16(p + 4), ru16(p + 6), ru16(p + 8)});
    }
  }
  for (i32 o = 0; o + 4 <= lv->size; o += 4) {
    const u8* p = &data_[size_t(lv->pos) + o];
    m.vertices.push_back({rd16(p), rd16(p + 2)});
  }
  for (i32 o = 0; o + 14 <= ll->size; o += 14) {
    const u8* p = &data_[size_t(ll->pos) + o];
    Linedef d{};
    d.v1 = ru16(p);
    d.v2 = ru16(p + 2);
    d.flags = ru16(p + 4);
    d.special = ru16(p + 6);
    d.tag = ru16(p + 8);
    d.side[0] = ru16(p + 10);
    d.side[1] = ru16(p + 12);
    m.linedefs.push_back(d);
  }
  for (i32 o = 0; o + 30 <= ls->size; o += 30) {
    const u8* p = &data_[size_t(ls->pos) + o];
    Sidedef s{};
    s.xoff = rd16(p);
    s.yoff = rd16(p + 2);
    s.top = name8(p + 4);
    s.bottom = name8(p + 12);
    s.mid = name8(p + 20);
    s.sector = ru16(p + 28);
    m.sidedefs.push_back(s);
  }
  for (i32 o = 0; o + 26 <= lsec->size; o += 26) {
    const u8* p = &data_[size_t(lsec->pos) + o];
    Sector s{};
    s.floor = rd16(p);
    s.ceiling = rd16(p + 2);
    s.floor_pic = name8(p + 4);
    s.ceil_pic = name8(p + 12);
    s.light = rd16(p + 20);
    s.special = ru16(p + 22);
    s.tag = ru16(p + 24);
    m.sectors.push_back(s);
  }
  *out = std::move(m);
  return true;
}

}  // namespace svx::doom

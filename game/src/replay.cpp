#include "svx/game/replay.hpp"

#include <cstring>

namespace svx {

namespace {

constexpr u32 kMagic = 0x4C585653;  // "SVXL"
constexpr u32 kVersion = 2;
constexpr u8 kMaxType = static_cast<u8>(Command::Type::Tunable);

template <typename T>
void put(std::vector<u8>& out, T v) {
  u8 b[sizeof(T)];
  std::memcpy(b, &v, sizeof(T));
  out.insert(out.end(), b, b + sizeof(T));
}

template <typename T>
bool get(const std::vector<u8>& in, size_t& at, T* v) {
  if (at + sizeof(T) > in.size()) return false;
  std::memcpy(v, in.data() + at, sizeof(T));
  at += sizeof(T);
  return true;
}

}  // namespace

std::vector<u8> CommandLog::serialize() const {
  std::vector<u8> out;
  out.reserve(16 + cmds_.size() * 57);
  put(out, kMagic);
  put(out, kVersion);
  put(out, static_cast<u64>(cmds_.size()));
  for (const Command& c : cmds_) {
    put(out, c.tick);
    put(out, static_cast<u8>(c.type));
    for (f64 v : c.a) put(out, v);
  }
  return out;
}

bool CommandLog::parse(const std::vector<u8>& bytes, CommandLog* out) {
  size_t at = 0;
  u32 magic = 0, version = 0;
  u64 n = 0;
  if (!get(bytes, at, &magic) || !get(bytes, at, &version) || !get(bytes, at, &n)) return false;
  if (magic != kMagic || version < 1 || version > kVersion || n > (bytes.size() - at) / 57) return false;
  CommandLog log;
  log.cmds_.reserve(n);
  i64 last = 0;
  for (u64 k = 0; k < n; ++k) {
    Command c;
    u8 type = 0;
    if (!get(bytes, at, &c.tick) || !get(bytes, at, &type)) return false;
    if (type < 1 || type > (version == 1 ? 5 : kMaxType) || c.tick < last) return false;  // unknown type or out of order
    c.type = static_cast<Command::Type>(type);
    for (f64& v : c.a)
      if (!get(bytes, at, &v)) return false;
    last = c.tick;
    log.cmds_.push_back(c);
  }
  if (at != bytes.size()) return false;
  *out = std::move(log);
  return true;
}

void apply_command(Game& e, const Command& c) {
  switch (c.type) {
    case Command::Type::Carve:
      e.carve({c.a[0], c.a[1], c.a[2]}, c.a[3]);
      break;
    case Command::Type::Blast:
      e.blast({c.a[0], c.a[1], c.a[2]}, c.a[3], c.a[4]);
      break;
    case Command::Type::Viewer:
      e.set_viewer({c.a[0], c.a[1], c.a[2]});
      break;
    case Command::Type::Use:
      e.use(V3{c.a[0], c.a[1], c.a[2]}, V3{c.a[3], c.a[4], c.a[5]});
      break;
    case Command::Type::Ignite:
      e.ignite({c.a[0], c.a[1], c.a[2]}, c.a[3]);
      break;
    case Command::Type::Extinguish:
      e.extinguish({c.a[0], c.a[1], c.a[2]}, c.a[3]);
      break;
    case Command::Type::Pour:
      e.pour({c.a[0], c.a[1], c.a[2]}, c.a[3]);
      break;
    case Command::Type::Heat:
      e.heat({c.a[0], c.a[1], c.a[2]}, c.a[3], c.a[4]);
      break;
    case Command::Type::Drain:
      e.drain({c.a[0], c.a[1], c.a[2]}, c.a[3]);
      break;
    case Command::Type::EnvParam:
      e.set_env(static_cast<i32>(c.a[0]), c.a[1]);
      break;
    case Command::Type::Tunable:
      e.set_tunable(static_cast<i32>(c.a[0]), c.a[1]);
      break;
    case Command::Type::Params: {
      GameParams p;
      p.fragility = c.a[0];
      p.impact = c.a[1];
      p.dif = c.a[2];
      p.debug_view = static_cast<int>(c.a[4]);
      p.paused = c.a[5] != 0.0;
      e.set_params(p);
      break;
    }
  }
}

}  // namespace svx

// structvox — host records (docs/CORE.md §3, Streaming): a host's state that belongs to a place,
// kept by the region archive while the place is out of range and handed back when it is known
// again - the way the world keeps its own pieces and articulations out of range.

#include <algorithm>
#include <cmath>

#include "archive.hpp"
#include "bytes.hpp"
#include "svx/world/world.hpp"
#include "world_internal.hpp"

namespace svx {

using world_detail::put3;
using world_detail::put32;
using world_detail::put64;
using world_detail::finite3;
using world_detail::Rd;

namespace {

constexpr u64 kRecordKey = (3ull << 62) | (1ull << 60);
constexpr u64 kRecordSerial = (1ull << 60) - 1;

}  // namespace

bool World::Impl::archive_host_record(u32 owner, u64 id, const V3& at, std::vector<u8> data) {
  if (!strm_.source || !strm_.archive || !finite3(at)) return false;
  const IVec3 c = chunk_of(world_detail::voxel_of(at, grid_.h));
  // (what it needs back: its chunk and the one it rests on, within the source's extent)
  std::vector<u64> chunks;
  for (i32 z = c[2] - 1; z <= c[2]; ++z) {
    const IVec3 q{c[0], c[1], z};
    bool inside = true;
    for (int a = 0; a < 3; ++a) inside = inside && q[a] >= strm_.lo[a] && q[a] < strm_.hi[a];
    if (inside) chunks.push_back(key3(q[0], q[1], q[2]));
  }
  const u64 key = kRecordKey | (strm_.next_record++ & kRecordSerial);
  std::vector<u8> rec;
  rec.reserve(36 + data.size());
  put32(rec, owner);
  put64(rec, id);
  put3(rec, at);
  rec.insert(rec.end(), data.begin(), data.end());
  archive_record(key, rec, region_of(key3(c[0], c[1], c[2])));
  if (!strm_.archive->has(key)) {
    ++st_.forgotten_records;  // (no room at all: it is gone)
    return false;
  }
  strm_.archived_records[key] = std::move(chunks);
  ++st_.archived_records;
  return true;
}

void World::Impl::restore_host_records() {
  if (strm_.archived_records.empty()) return;
  std::vector<u64> ready;
  for (const auto& [key, chunks] : strm_.archived_records)
    if (std::all_of(chunks.begin(), chunks.end(), [&](u64 k) { return chunk_known(k); })) ready.push_back(key);
  for (u64 key : ready) {
    strm_.archived_records.erase(key);
    --st_.archived_records;
    const std::vector<u8> rec = strm_.archive->get(key);
    strm_.archive->erase(key);
    Rd in{rec};
    HostRecord r;
    r.owner = in.u32_();
    r.id = in.u64_();
    r.at = in.v3();
    if (!in.ok) continue;  // (checked when made: never)
    r.data.assign(rec.begin() + static_cast<long>(in.p), rec.end());
    strm_.restored_records[r.owner].push_back(std::move(r));
  }
}

std::vector<HostRecord> World::Impl::take_host_records(u32 owner) {
  const auto it = strm_.restored_records.find(owner);
  if (it == strm_.restored_records.end()) return {};
  std::vector<HostRecord> out = std::move(it->second);
  strm_.restored_records.erase(it);
  return out;
}

bool World::Impl::forget_host_record(u64 key) {
  const auto it = strm_.archived_records.find(key);
  if (it == strm_.archived_records.end()) return false;
  strm_.archived_records.erase(it);
  --st_.archived_records;
  ++st_.forgotten_records;
  return true;
}

void World::Impl::write_host_records(std::vector<u8>& out) const {
  put64(out, strm_.next_record);
  put32(out, static_cast<u32>(strm_.archived_records.size()));
  for (const auto& [key, chunks] : strm_.archived_records) {
    put64(out, key);
    put32(out, static_cast<u32>(chunks.size()));
    for (u64 c : chunks) put64(out, c);
    const std::vector<u8> rec = strm_.archive->get(key);
    put32(out, static_cast<u32>(rec.size()));
    out.insert(out.end(), rec.begin(), rec.end());
  }
  size_t back = 0;
  for (const auto& [owner, rs] : strm_.restored_records) back += rs.size();
  put32(out, static_cast<u32>(back));
  for (const auto& [owner, rs] : strm_.restored_records)
    for (const HostRecord& r : rs) {
      put32(out, r.owner);
      put64(out, r.id);
      put3(out, r.at);
      put32(out, static_cast<u32>(r.data.size()));
      out.insert(out.end(), r.data.begin(), r.data.end());
    }
}

bool World::Impl::read_host_records(Rd& in, SessionDelta* s) const {
  s->host_records = true;
  s->next_record = in.u64_();
  const u32 n = in.u32_();
  if (!in.ok || u64(n) * 16 > in.b.size()) return false;
  for (u32 k = 0; k < n; ++k) {
    SessionDelta::ArchivedRecord a;
    a.key = in.u64_();
    const u32 nc = in.u32_();
    if (!in.ok || (a.key & ~kRecordSerial) != kRecordKey || u64(nc) * 8 > in.b.size()) return false;
    for (u32 q = 0; q < nc; ++q) a.chunks.push_back(in.u64_());
    const u32 sz = in.u32_();
    if (!in.ok || sz < 36 || !in.need(sz)) return false;
    a.record.assign(in.b.begin() + static_cast<long>(in.p), in.b.begin() + static_cast<long>(in.p + sz));
    in.p += sz;
    s->archived_records.push_back(std::move(a));
  }
  const u32 nb = in.u32_();
  if (!in.ok || u64(nb) * 40 > in.b.size()) return false;
  for (u32 k = 0; k < nb; ++k) {
    HostRecord r;
    r.owner = in.u32_();
    r.id = in.u64_();
    r.at = in.v3();
    const u32 sz = in.u32_();
    if (!in.ok || !in.need(sz)) return false;
    r.data.assign(in.b.begin() + static_cast<long>(in.p), in.b.begin() + static_cast<long>(in.p + sz));
    in.p += sz;
    s->restored_records.push_back(std::move(r));
  }
  return in.ok;
}

void World::Impl::apply_host_records(SessionDelta& s) {
  if (!s.host_records) return;
  for (const auto& [key, chunks] : strm_.archived_records) strm_.archive->erase(key);
  st_.archived_records = 0;
  strm_.archived_records.clear();
  strm_.restored_records.clear();
  strm_.next_record = std::max<u64>(1, s.next_record);
  for (HostRecord& r : s.restored_records) strm_.restored_records[r.owner].push_back(std::move(r));
  if (!strm_.source) return;
  for (SessionDelta::ArchivedRecord& a : s.archived_records) {
    Rd rin{a.record};
    rin.u32_();
    rin.u64_();
    const V3 at = rin.v3();
    const IVec3 c = chunk_of(world_detail::voxel_of(at, grid_.h));
    archive_record(a.key, a.record, region_of(key3(c[0], c[1], c[2])));
    if (!strm_.archive->has(a.key)) {
      ++st_.forgotten_records;
      continue;
    }
    strm_.archived_records[a.key] = std::move(a.chunks);
    ++st_.archived_records;
  }
}

}  // namespace svx

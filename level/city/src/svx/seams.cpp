// svx_city — the export's seams (svx/seams.hpp).
#include "svx/seams.hpp"

#include "svx/props.hpp"

namespace svx::city {

bool object_loose(const ChunkBuffer& c, uint32_t object) {
  if (object == 0 || object > c.objects.size()) return false;
  const ChunkBuffer::ObjectRef& o = c.objects[object - 1];
  const PropClass* pc = prop_class(o.group, o.kind);
  return pc && pc->attach == Attachment::Loose;
}

}  // namespace svx::city

// structvox — basic types and assertions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace svx {

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using f32 = float;
using f64 = double;

[[noreturn]] inline void fail(const char* file, int line, const char* what) {
  std::fprintf(stderr, "svx: fatal: %s:%d: %s\n", file, line, what);
  std::abort();
}

}  // namespace svx

#define SVX_ASSERT(cond)                                  \
  do {                                                    \
    if (!(cond)) ::svx::fail(__FILE__, __LINE__, #cond);  \
  } while (0)

#define SVX_FAIL(msg) ::svx::fail(__FILE__, __LINE__, msg)

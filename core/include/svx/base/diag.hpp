// structvox — developer diagnostics: printing to stdout, switched on by SVX_* environment
// variables (read once per name). They never change results. Builds with SVX_NO_DIAGNOSTICS
// defined compile them out.
#pragma once

#include <cstdlib>

namespace svx {

inline bool diag(const char* name) {
#ifdef SVX_NO_DIAGNOSTICS
  (void)name;
  return false;
#else
  return std::getenv(name) != nullptr;
#endif
}

}  // namespace svx

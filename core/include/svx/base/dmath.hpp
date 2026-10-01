// structvox — bundled deterministic math (plan §B9 determinism rules): the core's simulation
// (joint limits, kinematic drives) and the harness use it.
//
// Platform libms (Apple, glibc, musl in Emscripten, MSVC) may differ in the last bit of sin,
// cos, atan2, exp, log and pow. The solver uses these few functions through this header
// instead: ports of the fdlibm algorithms (Sun Microsystems, freely redistributable) built only
// from IEEE-754 basic operations (+ - * / and sqrt, which are correctly rounded everywhere), so
// results are bit-identical across native / WASM builds and x86 / ARM (with -ffp-contract=off).
// Accuracy: < 1 ulp for sin / cos / atan / exp / log; pow = exp(y log x) (a few ulp).
#pragma once

#include "svx/base/types.hpp"

namespace svx::dm {

f64 sin(f64 x);
f64 cos(f64 x);
f64 atan(f64 x);
f64 atan2(f64 y, f64 x);
f64 exp(f64 x);
f64 log(f64 x);
// x >= 0 (negative x: NaN unless y is an integer)
f64 pow(f64 x, f64 y);
// integer powers by repeated multiplication (exact order: deterministic)
inline f64 ipow(f64 x, int n) {
  f64 r = 1.0;
  const bool inv = n < 0;
  for (int k = 0; k < (inv ? -n : n); ++k) r *= x;
  return inv ? 1.0 / r : r;
}

}  // namespace svx::dm

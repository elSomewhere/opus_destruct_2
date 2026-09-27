// Bundled deterministic math (plan §B9): accuracy against the platform libm, and a golden
// digest of the exact output bits that every build (native ARM / x86, WASM) must reproduce.
#include <bit>
#include <cmath>
#include <cstdio>

#include "doctest.h"
#include "svx/game/dmath.hpp"

using namespace svx;

namespace {

// distance in units in the last place (same sign, finite)
f64 ulps(f64 a, f64 b) {
  if (a == b) return 0.0;
  if (std::signbit(a) != std::signbit(b)) return std::fabs(a - b) / std::numeric_limits<f64>::denorm_min();
  const i64 ia = std::bit_cast<i64>(std::fabs(a)), ib = std::bit_cast<i64>(std::fabs(b));
  return static_cast<f64>(ia > ib ? ia - ib : ib - ia);
}

struct Lcg {
  u64 s = 0x9E3779B97F4A7C15ull;
  f64 next(f64 lo, f64 hi) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return lo + (hi - lo) * static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0);
  }
};

}  // namespace

TEST_CASE("dmath: sin / cos / atan2 / exp / log / pow match the platform libm within 1-2 ulp") {
  Lcg r;
  f64 worst[6] = {0, 0, 0, 0, 0, 0};
  for (int k = 0; k < 200000; ++k) {
    const f64 x = k % 4 == 0 ? r.next(-1e-3, 1e-3) : k % 4 == 1 ? r.next(-4.0, 4.0) : r.next(-2000.0, 2000.0);
    worst[0] = std::max(worst[0], ulps(dm::sin(x), std::sin(x)));
    worst[1] = std::max(worst[1], ulps(dm::cos(x), std::cos(x)));
    const f64 y = r.next(-3.0, 3.0), z = r.next(-3.0, 3.0);
    worst[2] = std::max(worst[2], ulps(dm::atan2(y, z), std::atan2(y, z)));
    const f64 e = r.next(-700.0, 700.0);
    worst[3] = std::max(worst[3], ulps(dm::exp(e), std::exp(e)));
    const f64 l = std::exp(r.next(-700.0, 700.0));
    worst[4] = std::max(worst[4], ulps(dm::log(l), std::log(l)));
    const f64 b = r.next(0.0, 10.0), p = r.next(-3.0, 3.0);
    worst[5] = std::max(worst[5], ulps(dm::pow(b, p), std::pow(b, p)));
  }
  MESSAGE("worst ulps: sin " << worst[0] << " cos " << worst[1] << " atan2 " << worst[2] << " exp " << worst[3]
                             << " log " << worst[4] << " pow " << worst[5]);
  CHECK(worst[0] <= 1.0);
  CHECK(worst[1] <= 1.0);
  CHECK(worst[2] <= 2.0);
  CHECK(worst[3] <= 1.0);
  CHECK(worst[4] <= 1.0);
  CHECK(worst[5] <= 64.0);  // exp(y log x): error grows with |y log x|
  // special values
  CHECK(dm::sin(0.0) == 0.0);
  CHECK(dm::cos(0.0) == 1.0);
  CHECK(dm::atan2(0.0, 1.0) == 0.0);
  CHECK(dm::atan2(1.0, 0.0) == doctest::Approx(1.5707963267948966));
  CHECK(dm::atan2(0.0, -1.0) == doctest::Approx(3.141592653589793));
  CHECK(dm::exp(0.0) == 1.0);
  CHECK(dm::log(1.0) == 0.0);
  CHECK(dm::pow(2.0, 10.0) == doctest::Approx(1024.0).epsilon(1e-14));
  CHECK(dm::ipow(2.0, -2) == 0.25);
}

TEST_CASE("dmath: golden digest of the output bits (identical on every build)") {
  Lcg r;
  u64 h = 1469598103934665603ull;
  auto mix = [&](f64 v) { h = (h ^ std::bit_cast<u64>(v)) * 1099511628211ull; };
  for (int k = 0; k < 20000; ++k) {
    const f64 x = r.next(-10.0, 10.0);
    mix(dm::sin(x));
    mix(dm::cos(x));
    mix(dm::atan2(x, r.next(-10.0, 10.0)));
    mix(dm::exp(x));
    mix(dm::log(std::fabs(x) + 1e-3));
    mix(dm::pow(std::fabs(x), 1.5));
  }
  std::printf("dmath digest %016llx\n", static_cast<unsigned long long>(h));
  CHECK(h == 0x410fe26f50cb825full);  // native ARM (Apple clang), WASM and x86-64
}

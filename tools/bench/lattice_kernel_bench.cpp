// Matrix-free 6-DOF rigid-cell lattice operator y = K u (face bonds, axis-aligned lever arms).
// Dense box of cells with a per-bond mask (models sparse structure within bricks).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>

template <typename T>
struct Field6 { std::vector<T> c[6]; void init(size_t n) { for (auto& v : c) v.assign(n, T(0)); } };

template <typename T>
static void apply_axis(int a, int nx, int ny, int nz, const T* k, T hh,
                       const Field6<T>& u, Field6<T>& y, const T* mask, int z0, int z1) {
  const int t1 = (a + 1) % 3, t2 = (a + 2) % 3;
  const long sx = 1, sy = nx, sz = (long)nx * ny;
  const long stride = a == 0 ? sx : (a == 1 ? sy : sz);
  const T kn = k[0], ks = k[1], kt = k[3], kb = k[4];
  for (int z = z0; z < z1; ++z) {
    if (a == 2 && z == nz - 1) continue;
    for (int yy = 0; yy < ny; ++yy) {
      if (a == 1 && yy == ny - 1) continue;
      const long row = z * sz + yy * sy;
      const int xend = a == 0 ? nx - 1 : nx;
      const T* ua = u.c[a].data() + row;  const T* ut1 = u.c[t1].data() + row;  const T* ut2 = u.c[t2].data() + row;
      const T* ra = u.c[3 + a].data() + row; const T* rt1 = u.c[3 + t1].data() + row; const T* rt2 = u.c[3 + t2].data() + row;
      T* ya = y.c[a].data() + row;  T* yt1 = y.c[t1].data() + row;  T* yt2 = y.c[t2].data() + row;
      T* qa = y.c[3 + a].data() + row; T* qt1 = y.c[3 + t1].data() + row; T* qt2 = y.c[3 + t2].data() + row;
      const T* m = mask + (size_t)a * ((size_t)nx * ny * nz) + row;
      if (a != 0) {
        // j = i + stride lies in a different row: compute row of bond forces, then scatter to both rows
        T f[6][1024];
        const T* __restrict ua_j = ua + stride; const T* __restrict ut1_j = ut1 + stride; const T* __restrict ut2_j = ut2 + stride;
        const T* __restrict ra_j = ra + stride; const T* __restrict rt1_j = rt1 + stride; const T* __restrict rt2_j = rt2 + stride;
#pragma clang loop vectorize(enable)
        for (int x = 0; x < xend; ++x) {
          const T w = m[x];
          f[0][x] = w * kn * (ua_j[x] - ua[x]);
          f[1][x] = w * ks * (ut1_j[x] - ut1[x] - hh * (rt2[x] + rt2_j[x]));
          f[2][x] = w * ks * (ut2_j[x] - ut2[x] + hh * (rt1[x] + rt1_j[x]));
          f[3][x] = w * kt * (ra_j[x] - ra[x]);
          f[4][x] = w * kb * (rt1_j[x] - rt1[x]);
          f[5][x] = w * kb * (rt2_j[x] - rt2[x]);
        }
        T* __restrict ya_i = ya; T* __restrict yt1_i = yt1; T* __restrict yt2_i = yt2;
        T* __restrict qa_i = qa; T* __restrict qt1_i = qt1; T* __restrict qt2_i = qt2;
#pragma clang loop vectorize(enable)
        for (int x = 0; x < xend; ++x) {
          ya_i[x] -= f[0][x]; yt1_i[x] -= f[1][x]; yt2_i[x] -= f[2][x]; qa_i[x] -= f[3][x];
          qt1_i[x] += -f[4][x] + hh * f[2][x]; qt2_i[x] += -f[5][x] - hh * f[1][x];
        }
        T* __restrict ya_j = ya + stride; T* __restrict yt1_j = yt1 + stride; T* __restrict yt2_j = yt2 + stride;
        T* __restrict qa_j = qa + stride; T* __restrict qt1_j = qt1 + stride; T* __restrict qt2_j = qt2 + stride;
#pragma clang loop vectorize(enable)
        for (int x = 0; x < xend; ++x) {
          ya_j[x] += f[0][x]; yt1_j[x] += f[1][x]; yt2_j[x] += f[2][x]; qa_j[x] += f[3][x];
          qt1_j[x] += f[4][x] + hh * f[2][x]; qt2_j[x] += f[5][x] - hh * f[1][x];
        }
      } else {
        // x-bonds: compute bond forces for the row, then scatter with shifted adds
        T f[6][1024];
#pragma clang loop vectorize(enable)
        for (int x = 0; x < xend; ++x) {
          const int j = x + 1; const T w = m[x];
          f[0][x] = w * kn * (ua[j] - ua[x]);
          f[1][x] = w * ks * (ut1[j] - ut1[x] - hh * (rt2[x] + rt2[j]));
          f[2][x] = w * ks * (ut2[j] - ut2[x] + hh * (rt1[x] + rt1[j]));
          f[3][x] = w * kt * (ra[j] - ra[x]);
          f[4][x] = w * kb * (rt1[j] - rt1[x]);
          f[5][x] = w * kb * (rt2[j] - rt2[x]);
        }
#pragma clang loop vectorize(enable)
        for (int x = 0; x < xend; ++x) {
          ya[x] -= f[0][x]; yt1[x] -= f[1][x]; yt2[x] -= f[2][x]; qa[x] -= f[3][x];
          qt1[x] += -f[4][x] + hh * f[2][x]; qt2[x] += -f[5][x] - hh * f[1][x];
        }
#pragma clang loop vectorize(enable)
        for (int x = 0; x < xend; ++x) {
          ya[x + 1] += f[0][x]; yt1[x + 1] += f[1][x]; yt2[x + 1] += f[2][x]; qa[x + 1] += f[3][x];
          qt1[x + 1] += f[4][x] + hh * f[2][x]; qt2[x + 1] += f[5][x] - hh * f[1][x];
        }
      }
    }
  }
}

template <typename T>
double bench(int n, int reps, int threads) {
  const size_t N = (size_t)n * n * n;
  Field6<T> u, y; u.init(N); y.init(N);
  std::vector<T> mask(3 * N);
  unsigned s = 12345;
  for (auto& v : mask) { s = s * 1103515245u + 12345u; v = ((s >> 16) & 7) ? T(1) : T(0); }  // ~87% bonds intact
  for (int c = 0; c < 6; ++c) for (size_t i = 0; i < N; ++i) { s = s * 1103515245u + 12345u; u.c[c][i] = T((s >> 8) & 1023) / T(1024); }
  const T k[6] = {T(1000), T(350), T(350), T(50), T(80), T(80)};
  const T hh = T(0.5);
  auto run = [&](int z0, int z1) {
    for (int a = 0; a < 3; ++a) apply_axis<T>(a, n, n, n, k, hh, u, y, mask.data(), z0, z1);
  };
  auto t0 = std::chrono::high_resolution_clock::now();
  for (int r = 0; r < reps; ++r) {
    for (int c = 0; c < 6; ++c) std::fill(y.c[c].begin(), y.c[c].end(), T(0));
    if (threads <= 1) { run(0, n); }
    else {
      // two-phase slab partition in z (even slabs then odd slabs) avoids write races on z-bonds
      const int slabs = threads * 2; std::vector<std::thread> th;
      for (int phase = 0; phase < 2; ++phase) {
        th.clear();
        for (int t = 0; t < threads; ++t) {
          int sidx = 2 * t + phase; int z0 = sidx * n / slabs, z1 = (sidx + 1) * n / slabs;
          th.emplace_back([&, z0, z1] { run(z0, z1); });
        }
        for (auto& x : th) x.join();
      }
    }
  }
  auto t1 = std::chrono::high_resolution_clock::now();
  double sec = std::chrono::duration<double>(t1 - t0).count();
  double chk = 0; for (int c = 0; c < 6; ++c) for (size_t i = 0; i < N; i += 97) chk += y.c[c][i];
  std::printf("  %s n=%d^3 (%zu cells) threads=%d: %.2f ns/cell/matvec  (%.1f Mcells/s)  chk=%.3e\n",
              sizeof(T) == 4 ? "f32" : "f64", n, N, threads, sec / reps / N * 1e9, reps * N / sec / 1e6, chk);
  return sec;
}

int main(int argc, char** argv) {
  int reps = argc > 1 ? std::atoi(argv[1]) : 20;
  for (int n : {32, 60, 96}) { bench<float>(n, reps, 1); bench<double>(n, reps, 1); }
  unsigned hw = std::thread::hardware_concurrency();
  for (int t : {4, 8}) if ((unsigned)t <= hw) bench<float>(96, reps, t);
  return 0;
}

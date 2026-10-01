// svx_city — V8's Array.prototype.sort (third_party/v8/builtins/array-sort.tq), included by
// core/js.hpp. A port, step for step: the same comparisons in the same order, so the result is
// V8's whatever the comparator returns (SortCompare: a NaN result counts as 0).
#pragma once

#include <algorithm>

namespace svx::city::js::detail {

template <class T, class Cmp>
int TimSort<T, Cmp>::count_and_make_run(int lo_arg, int hi) {
  const int lo = lo_arg + 1;
  if (lo == hi) return 1;
  int run = 2;
  double order = c(a_[lo], a_[lo - 1]);
  const bool desc = order < 0;
  int prev = lo;
  for (int idx = lo + 1; idx < hi; ++idx) {
    order = c(a_[idx], a_[prev]);
    if (desc) {
      if (order >= 0) break;
    } else {
      if (order < 0) break;
    }
    prev = idx;
    ++run;
  }
  if (desc) {
    int l = lo_arg, h = lo_arg + run - 1;
    while (l < h) std::swap(a_[l++], a_[h--]);
  }
  return run;
}

template <class T, class Cmp>
void TimSort<T, Cmp>::binary_insertion_sort(int lo, int start_arg, int hi) {
  int start = lo == start_arg ? start_arg + 1 : start_arg;
  for (; start < hi; ++start) {
    int left = lo, right = start;
    T pivot = std::move(a_[start]);
    while (left < right) {
      const int mid = left + ((right - left) >> 1);
      const double order = c(pivot, a_[mid]);
      if (order < 0)
        right = mid;
      else
        left = mid + 1;
    }
    for (int p = start; p > left; --p) a_[p] = std::move(a_[p - 1]);
    a_[left] = std::move(pivot);
  }
}

template <class T, class Cmp>
void TimSort<T, Cmp>::merge_collapse() {
  auto len = [&](int i) { return runs_[i].len; };
  auto established = [&](int n) { return n < 2 || len(n - 2) > len(n - 1) + len(n); };
  while (runs_.size() > 1) {
    int n = static_cast<int>(runs_.size()) - 2;
    if (!established(n + 1) || !established(n)) {
      if (len(n - 1) < len(n + 1)) --n;
      merge_at(n);
    } else if (len(n) <= len(n + 1)) {
      merge_at(n);
    } else {
      break;
    }
  }
}

template <class T, class Cmp>
void TimSort<T, Cmp>::merge_force_collapse() {
  while (runs_.size() > 1) {
    int n = static_cast<int>(runs_.size()) - 2;
    if (n > 0 && runs_[n - 1].len < runs_[n + 1].len) --n;
    merge_at(n);
  }
}

template <class T, class Cmp>
void TimSort<T, Cmp>::merge_at(int i) {
  const int size = static_cast<int>(runs_.size());
  int base_a = runs_[i].base, len_a = runs_[i].len;
  const int base_b = runs_[i + 1].base;
  int len_b = runs_[i + 1].len;
  runs_[i].len = len_a + len_b;
  if (i == size - 3) runs_[i + 1] = runs_[i + 2];
  runs_.pop_back();
  const int k = gallop_right(a_[base_b], a_.data() + base_a, len_a, 0);
  base_a += k;
  len_a -= k;
  if (len_a == 0) return;
  len_b = gallop_left(a_[base_a + len_a - 1], a_.data() + base_b, len_b, len_b - 1);
  if (len_b == 0) return;
  if (len_a <= len_b)
    merge_low(base_a, len_a, base_b, len_b);
  else
    merge_high(base_a, len_a, base_b, len_b);
}

template <class T, class Cmp>
int TimSort<T, Cmp>::gallop_left(const T& key, const T* base, int len, int hint) {
  int last = 0, ofs = 1;
  double order = c(base[hint], key);
  if (order < 0) {
    const int max_ofs = len - hint;
    while (ofs < max_ofs) {
      order = c(base[hint + ofs], key);
      if (order >= 0) break;
      last = ofs;
      ofs = (ofs << 1) + 1;
      if (ofs <= 0) ofs = max_ofs;
    }
    if (ofs > max_ofs) ofs = max_ofs;
    last += hint;
    ofs += hint;
  } else {
    const int max_ofs = hint + 1;
    while (ofs < max_ofs) {
      order = c(base[hint - ofs], key);
      if (order < 0) break;
      last = ofs;
      ofs = (ofs << 1) + 1;
      if (ofs <= 0) ofs = max_ofs;
    }
    if (ofs > max_ofs) ofs = max_ofs;
    const int tmp = last;
    last = hint - ofs;
    ofs = hint - tmp;
  }
  ++last;
  while (last < ofs) {
    const int m = last + ((ofs - last) >> 1);
    order = c(base[m], key);
    if (order < 0)
      last = m + 1;
    else
      ofs = m;
  }
  return ofs;
}

template <class T, class Cmp>
int TimSort<T, Cmp>::gallop_right(const T& key, const T* base, int len, int hint) {
  int last = 0, ofs = 1;
  double order = c(key, base[hint]);
  if (order < 0) {
    const int max_ofs = hint + 1;
    while (ofs < max_ofs) {
      order = c(key, base[hint - ofs]);
      if (order >= 0) break;
      last = ofs;
      ofs = (ofs << 1) + 1;
      if (ofs <= 0) ofs = max_ofs;
    }
    if (ofs > max_ofs) ofs = max_ofs;
    const int tmp = last;
    last = hint - ofs;
    ofs = hint - tmp;
  } else {
    const int max_ofs = len - hint;
    while (ofs < max_ofs) {
      order = c(key, base[hint + ofs]);
      if (order < 0) break;
      last = ofs;
      ofs = (ofs << 1) + 1;
      if (ofs <= 0) ofs = max_ofs;
    }
    if (ofs > max_ofs) ofs = max_ofs;
    last += hint;
    ofs += hint;
  }
  ++last;
  while (last < ofs) {
    const int m = last + ((ofs - last) >> 1);
    order = c(key, base[m]);
    if (order < 0)
      ofs = m;
    else
      last = m + 1;
  }
  return ofs;
}

// (V8's Copy: overlapping ranges are copied as by memmove)
template <class T>
inline void move_range(T* src, T* dst, int n) {
  if (n <= 0 || src == dst) return;
  if (src < dst)
    std::move_backward(src, src + n, dst + n);
  else
    std::move(src, src + n, dst);
}

template <class T, class Cmp>
void TimSort<T, Cmp>::merge_low(int base_a, int len_a, int base_b, int len_b) {
  constexpr int kMinGallopWins = 7;
  tmp_.resize(static_cast<size_t>(len_a));
  T* w = a_.data();
  T* t = tmp_.data();
  move_range(w + base_a, t, len_a);
  int dest = base_a, ct = 0, cb = base_b;
  w[dest++] = std::move(w[cb++]);
  if (--len_b == 0) goto succeed;
  if (len_a == 1) goto copy_b;
  {
    int min_gallop = min_gallop_;
    for (;;) {
      int wins_a = 0, wins_b = 0;
      for (;;) {
        const double order = c(w[cb], t[ct]);
        if (order < 0) {
          w[dest++] = std::move(w[cb++]);
          ++wins_b;
          --len_b;
          wins_a = 0;
          if (len_b == 0) goto succeed;
          if (wins_b >= min_gallop) break;
        } else {
          w[dest++] = std::move(t[ct++]);
          ++wins_a;
          --len_a;
          wins_b = 0;
          if (len_a == 1) goto copy_b;
          if (wins_a >= min_gallop) break;
        }
      }
      ++min_gallop;
      bool first = true;
      while (wins_a >= kMinGallopWins || wins_b >= kMinGallopWins || first) {
        first = false;
        min_gallop = std::max(1, min_gallop - 1);
        min_gallop_ = min_gallop;
        wins_a = gallop_right(w[cb], t + ct, len_a, 0);
        if (wins_a > 0) {
          move_range(t + ct, w + dest, wins_a);
          dest += wins_a;
          ct += wins_a;
          len_a -= wins_a;
          if (len_a == 1) goto copy_b;
          if (len_a == 0) goto succeed;
        }
        w[dest++] = std::move(w[cb++]);
        if (--len_b == 0) goto succeed;
        wins_b = gallop_left(t[ct], w + cb, len_b, 0);
        if (wins_b > 0) {
          move_range(w + cb, w + dest, wins_b);
          dest += wins_b;
          cb += wins_b;
          len_b -= wins_b;
          if (len_b == 0) goto succeed;
        }
        w[dest++] = std::move(t[ct++]);
        if (--len_a == 1) goto copy_b;
      }
      ++min_gallop;
      min_gallop_ = min_gallop;
    }
  }
succeed:
  if (len_a > 0) move_range(t + ct, w + dest, len_a);
  return;
copy_b:
  move_range(w + cb, w + dest, len_b);
  w[dest + len_b] = std::move(t[ct]);
}

template <class T, class Cmp>
void TimSort<T, Cmp>::merge_high(int base_a, int len_a, int base_b, int len_b) {
  constexpr int kMinGallopWins = 7;
  tmp_.resize(static_cast<size_t>(len_b));
  T* w = a_.data();
  T* t = tmp_.data();
  move_range(w + base_b, t, len_b);
  int dest = base_b + len_b - 1, ct = len_b - 1, ca = base_a + len_a - 1;
  w[dest--] = std::move(w[ca--]);
  if (--len_a == 0) goto succeed;
  if (len_b == 1) goto copy_a;
  {
    int min_gallop = min_gallop_;
    for (;;) {
      int wins_a = 0, wins_b = 0;
      for (;;) {
        const double order = c(t[ct], w[ca]);
        if (order < 0) {
          w[dest--] = std::move(w[ca--]);
          ++wins_a;
          --len_a;
          wins_b = 0;
          if (len_a == 0) goto succeed;
          if (wins_a >= min_gallop) break;
        } else {
          w[dest--] = std::move(t[ct--]);
          ++wins_b;
          --len_b;
          wins_a = 0;
          if (len_b == 1) goto copy_a;
          if (wins_b >= min_gallop) break;
        }
      }
      ++min_gallop;
      bool first = true;
      while (wins_a >= kMinGallopWins || wins_b >= kMinGallopWins || first) {
        first = false;
        min_gallop = std::max(1, min_gallop - 1);
        min_gallop_ = min_gallop;
        int k = gallop_right(t[ct], w + base_a, len_a, len_a - 1);
        wins_a = len_a - k;
        if (wins_a > 0) {
          dest -= wins_a;
          ca -= wins_a;
          move_range(w + ca + 1, w + dest + 1, wins_a);
          len_a -= wins_a;
          if (len_a == 0) goto succeed;
        }
        w[dest--] = std::move(t[ct--]);
        if (--len_b == 1) goto copy_a;
        k = gallop_left(w[ca], t, len_b, len_b - 1);
        wins_b = len_b - k;
        if (wins_b > 0) {
          dest -= wins_b;
          ct -= wins_b;
          move_range(t + ct + 1, w + dest + 1, wins_b);
          len_b -= wins_b;
          if (len_b == 1) goto copy_a;
          if (len_b == 0) goto succeed;
        }
        w[dest--] = std::move(w[ca--]);
        if (--len_a == 0) goto succeed;
      }
      ++min_gallop;
      min_gallop_ = min_gallop;
    }
  }
succeed:
  if (len_b > 0) move_range(t, w + dest - (len_b - 1), len_b);
  return;
copy_a:
  dest -= len_a;
  ca -= len_a;
  move_range(w + ca + 1, w + dest + 1, len_a);
  w[dest] = std::move(t[ct]);
}

template <class T, class Cmp>
void TimSort<T, Cmp>::run() {
  const int length = static_cast<int>(a_.size());
  if (length < 2) return;
  int remaining = length, low = 0;
  const int min_run_len = min_run(remaining);
  while (remaining != 0) {
    int cur = count_and_make_run(low, low + remaining);
    if (cur < min_run_len) {
      const int forced = std::min(min_run_len, remaining);
      binary_insertion_sort(low, low + cur, low + forced);
      cur = forced;
    }
    runs_.push_back({low, cur});
    merge_collapse();
    low += cur;
    remaining -= cur;
  }
  merge_force_collapse();
}

}  // namespace svx::city::js::detail

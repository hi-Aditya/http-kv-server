// Store micro-benchmark: kvx::Store vs std::unordered_map<string,string>, single thread.
//   cmake --preset release && cmake --build --preset release --target bench_store
//   taskset -c 2 build/release/bench_store [N=1000000] [value_bytes=64] [reps=5] | tee bench/results/store.csv
// Pin the CPU, set the governor to `performance`, close other work. Output is CSV; the median of
// `reps` fresh runs is reported. `kvx-evict` caps memory at half the data so inserts evict
// (its heap_bytes_per_key divides by keys inserted, not keys retained: not comparable).
#include <malloc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "store/store.hpp"

using Clock = std::chrono::steady_clock;
static size_t g_sink = 0;  // defeats dead-code elimination

struct StdMap {
  std::unordered_map<std::string, std::string> m;
  void put(const std::string& k, const std::string& v) { m[k] = v; }
  void get(const std::string& k) { auto it = m.find(k); if (it != m.end()) g_sink += it->second.size(); }
};
struct Kvx {
  kvx::Store s;
  explicit Kvx(size_t limit) : s(kvx::StoreConfig{limit, 0xBEEF}) {}
  void put(const std::string& k, const std::string& v) { g_sink += static_cast<size_t>(s.put(k, v, 0, 0)); }
  void get(const std::string& k) { std::string_view v; if (s.get(k, 0, v)) g_sink += v.size(); }
};

static size_t heap_in_use() { return mallinfo2().uordblks; }
static double median(std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
template <class F> static double ns_per_op(size_t n, F&& f) {
  auto t0 = Clock::now();
  f();
  return std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / static_cast<double>(n);
}

template <class Make>
static void run(const char* name, Make make, const std::vector<std::string>& keys,
                const std::string& val, int reps) {
  const size_t n = keys.size();
  std::mt19937_64 rng(7);
  std::vector<uint32_t> order(n);
  std::iota(order.begin(), order.end(), 0u);
  std::shuffle(order.begin(), order.end(), rng);
  std::vector<double> ins, get, mix, bpk;
  for (int r = 0; r < reps; ++r) {
    size_t before = heap_in_use();
    auto st = make();
    ins.push_back(ns_per_op(n, [&] { for (auto& k : keys) st->put(k, val); }));
    bpk.push_back(static_cast<double>(heap_in_use() - before) / static_cast<double>(n));
    get.push_back(ns_per_op(n, [&] { for (uint32_t i : order) st->get(keys[i]); }));
    mix.push_back(ns_per_op(n, [&] {  // 90% get / 10% overwrite
      for (size_t j = 0; j < n; ++j) { const auto& k = keys[order[j]]; if (j % 10 == 0) st->put(k, val); else st->get(k); }
    }));
  }
  std::printf("%s,%zu,%zu,%.1f,%.1f,%.1f,%.1f\n", name, n, val.size(), median(ins), median(get), median(mix), median(bpk));
}

int main(int argc, char** argv) {
  size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000;
  size_t vs = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 64;
  int reps = argc > 3 ? std::atoi(argv[3]) : 5;
  if (n == 0 || reps <= 0) return 2;
  std::vector<std::string> keys(n);
  for (size_t i = 0; i < n; ++i) keys[i] = "user:" + std::to_string(i * 2654435761u);
  const std::string val(vs, 'v');
  const size_t data = n * kvx::Store::entry_cost(16, vs);
  const size_t full = data + n * 32 + (1u << 20);  // entries + bucket array (<= 16 B/key) + slack

  std::puts("impl,keys,value_bytes,insert_ns,get_hit_ns,mixed_90_10_ns,heap_bytes_per_key");
  run("std::unordered_map", [] { return std::make_unique<StdMap>(); }, keys, val, reps);
  run("kvx", [&] { return std::make_unique<Kvx>(full); }, keys, val, reps);
  run("kvx-evict", [&] { return std::make_unique<Kvx>(data / 2); }, keys, val, reps);
  if (g_sink == 42) std::puts("");  // keep g_sink observable
}

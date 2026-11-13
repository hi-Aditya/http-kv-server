// Store unit tests: deterministic (fixed seeds, caller-supplied clock), checked against a model.
#include <cstdio>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>

#include "store/store.hpp"

using namespace kvx;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static std::optional<std::string> get(Store& s, const std::string& k, int64_t now = 0) {
  std::string_view v;
  if (!s.get(k, now, v)) return std::nullopt;
  return std::string(v);
}
static Store make(size_t limit) { return Store(StoreConfig{limit, 0x1234}); }

static void basics() {
  Store s = make(1 << 20);
  CHECK(s.put("a", "1", 0, 0) == PutResult::Created);
  CHECK(s.put("a", "2", 0, 0) == PutResult::Replaced);                 // same size class: in place
  CHECK(s.put("a", std::string(500, 'x'), 0, 0) == PutResult::Replaced);  // new size class
  CHECK(get(s, "a") == std::string(500, 'x'));
  CHECK(s.put("e", "", 0, 0) == PutResult::Created && get(s, "e") == "");
  CHECK(s.del("a", 0) && !s.del("a", 0) && !get(s, "a"));
  CHECK(s.stats().keys == 1 && s.check());
}

// Random ops vs std::unordered_map. Without pressure the two must agree exactly; under a
// tiny cap the store may only hold a subset, but every value it returns must be the latest.
static void differential(size_t limit, bool pressure) {
  Store s = make(limit);
  std::unordered_map<std::string, std::string> model;
  std::mt19937_64 rng(42);
  for (int i = 0; i < 200000; ++i) {
    std::string k = "key" + std::to_string(rng() % 5000);
    switch (rng() % 4) {
      case 0: case 1: {
        std::string v(rng() % 512, static_cast<char>('a' + rng() % 26));
        PutResult r = s.put(k, v, 0, 0);
        CHECK(r == PutResult::Created || r == PutResult::Replaced);
        model[k] = v;
        break;
      }
      case 2: {
        auto got = get(s, k);
        if (got) CHECK(model.count(k) && model[k] == *got);
        else if (!pressure) CHECK(!model.count(k));
        break;
      }
      default: {
        bool had = s.del(k, 0);
        if (had) CHECK(model.count(k));
        if (!pressure) CHECK(had == (model.count(k) == 1));
        model.erase(k);
      }
    }
    CHECK(s.stats().mem_used <= limit);
    if (i % 5000 == 0) CHECK(s.check());
  }
  CHECK(s.check());
  if (!pressure) {
    CHECK(s.stats().keys == model.size() && s.stats().evictions == 0);
    for (auto& [k, v] : model) CHECK(get(s, k) == v);
  } else {
    CHECK(s.stats().evictions > 0);
  }
}

// CLOCK second chance: keys touched between inserts survive a flood many times the capacity;
// without touches the same keys do not.
static size_t hot_survivors(bool touch) {
  Store s = make(512 * 1024);
  const std::string v(100, 'v');
  for (int i = 0; i < 200; ++i) s.put("hot" + std::to_string(i), v, 0, 0);
  for (int i = 0; i < 20000; ++i) {
    if (touch && i % 50 == 0)
      for (int h = 0; h < 200; ++h) get(s, "hot" + std::to_string(h));
    s.put("cold" + std::to_string(i), v, 0, 0);
    if (s.stats().mem_used > 512 * 1024) { CHECK(false); break; }
  }
  size_t n = 0;
  for (int h = 0; h < 200; ++h) n += get(s, "hot" + std::to_string(h)).has_value();
  CHECK(s.stats().evictions > 10000 && s.check());
  return n;
}

static void ttl() {
  Store s = make(1 << 20);
  s.put("k", "v", 1000, 5000);                               // expires at 6000
  CHECK(get(s, "k", 5999) == "v");
  CHECK(!get(s, "k", 6000) && s.stats().keys == 0 && s.stats().expired == 1 && s.check());  // lazy
  s.put("k", "v", 1000, 0);
  CHECK(s.put("k", "w", 0, 0) == PutResult::Replaced);       // overwrite without ttl clears it
  CHECK(get(s, "k", 1u << 30) == "w" && s.check());
  s.put("k", "x", 1000, 0);
  s.put("k", "y", 3000, 500);                                // new ttl replaces old: expires at 3500
  CHECK(get(s, "k", 1500) == "y" && !get(s, "k", 3500));
  s.put("d", "v", 10, 0);
  CHECK(!s.del("d", 10));                                    // deleting an expired key reports absent
  s.put("z", "old", 10, 0);
  CHECK(s.put("z", "new", 0, 20) == PutResult::Created && get(s, "z", 99) == "new");  // expired -> fresh
  CHECK(s.check());
}

static void active_sweep() {
  Store s = make(16 << 20);
  for (int i = 0; i < 5000; ++i) s.put("t" + std::to_string(i), "v", 10, 0);
  for (int i = 0; i < 1000; ++i) s.put("p" + std::to_string(i), "v", 0, 0);
  CHECK(s.sweep(20, 1) < 50);  // one bucket: strictly bounded work
  int calls = 0;
  while (s.stats().keys > 1000 && calls < 1000) { s.sweep(20, 256); ++calls; }
  CHECK(s.stats().keys == 1000 && calls <= static_cast<int>(s.stats().buckets / 256) + 2);
  CHECK(s.sweep(20, 1 << 20) == 0 && s.check());
  CHECK(s.stats().expired == 5000);
}

static void cap_and_growth() {
  Store s = make(64 * 1024);
  s.put("keep", "small", 0, 0);
  CHECK(s.put("big", std::string(70000, 'x'), 0, 0) == PutResult::TooLarge);
  CHECK(get(s, "keep") == "small" && !get(s, "big"));        // rejected before any eviction
  CHECK(s.put("big2", std::string(40000, 'x'), 0, 0) == PutResult::Created && s.check());

  Store g = make(256u << 20);                                // growth keeps chains short
  for (int i = 0; i < 200000; ++i) g.put("key" + std::to_string(i), "v", 0, 0);
  size_t chain = 0;
  CHECK(g.check(&chain) && chain <= 16 && g.stats().buckets >= 131072);
  for (int i = 0; i < 200000; i += 997) CHECK(get(g, "key" + std::to_string(i)) == "v");

  Store u = make(64 * 1024);                                 // old+new bucket arrays would not fit
  for (int i = 0; i < 5000; ++i) u.put("k" + std::to_string(i), "v", 0, 0);
  CHECK(u.check() && u.stats().buckets == 1024 && u.stats().keys > 1024);

  Store t = make(1 << 20);                                   // growth is refused under a tight cap
  for (int i = 0; i < 100000; ++i) t.put("key" + std::to_string(i), "v", 0, 0);
  CHECK(t.check() && t.stats().mem_used <= (1u << 20) && t.stats().buckets * 8 <= (1u << 20) / 4);
}

int main() {
  basics();
  differential(64u << 20, false);
  differential(256 * 1024, true);
  CHECK(hot_survivors(true) == 200);
  CHECK(hot_survivors(false) < 100);
  ttl();
  active_sweep();
  cap_and_growth();
  std::printf(g_fail ? "%d FAILED\n" : "all store tests passed\n", g_fail);
  return g_fail ? 1 : 0;
}

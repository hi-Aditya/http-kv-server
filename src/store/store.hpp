#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kvx {

struct StoreConfig {
  size_t mem_limit = 256u << 20;  // bytes for entries + bucket array (see entry_cost)
  uint64_t seed = 0;
};

enum class PutResult { Created, Replaced, TooLarge, NoMemory };

class Store {
 public:
  struct Stats {
    size_t keys, buckets, mem_used, mem_limit;
    uint64_t evictions, expired;
  };

  explicit Store(const StoreConfig& cfg);  // throws std::invalid_argument / std::bad_alloc
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // `value` stays valid until the next mutating call. A hit marks the entry recently used.
  bool get(std::string_view key, int64_t now_ms, std::string_view& value);
  // ttl_ms == 0: no expiry (an overwrite replaces any previous TTL). May evict other entries.
  PutResult put(std::string_view key, std::string_view value, int64_t ttl_ms, int64_t now_ms);
  bool del(std::string_view key, int64_t now_ms);
  size_t sweep(int64_t now_ms, size_t max_buckets);

  Stats stats() const;
  bool check(size_t* max_chain = nullptr) const;  // walks the whole table; for tests
  static size_t entry_cost(size_t klen, size_t vlen);

 private:
  struct Entry;
  Entry** find(uint64_t h, std::string_view key) const;
  void release(Entry* e);
  void remove(Entry** pp);
  bool evict_one(const Entry* protect, int64_t now_ms);
  void maybe_grow();

  Entry** buckets_ = nullptr;
  size_t nbuckets_ = 0, mask_ = 0;
  size_t count_ = 0, used_ = 0, limit_ = 0, ttl_count_ = 0;
  size_t hand_ = 0, sweep_pos_ = 0;  // CLOCK hand and TTL sweep cursor (bucket indices)
  uint64_t seed_ = 0, evictions_ = 0, expired_ = 0;
};

}  // namespace kvx

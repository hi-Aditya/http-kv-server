#include "store/store.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

#include "util/hash.hpp"

namespace kvx {
namespace {
constexpr size_t kInitialBuckets = 1024;
constexpr size_t kMinLimit = 64 * 1024;
}  // namespace

struct Store::Entry {
  Entry* next;
  uint64_t hash;
  int64_t expire;  // unix ms; 0 = never
  uint32_t vlen;
  uint16_t klen;
  uint8_t ref;  // CLOCK reference bit
  uint8_t pad_;
  char* key() { return reinterpret_cast<char*>(this) + sizeof(Entry); }
  const char* key() const { return reinterpret_cast<const char*>(this) + sizeof(Entry); }
  char* val() { return key() + klen; }
  size_t cost() const { return entry_cost(klen, vlen); }
  bool expired(int64_t now) const { return expire != 0 && expire <= now; }
};
size_t Store::entry_cost(size_t klen, size_t vlen) {
  static_assert(sizeof(Entry) == 32);
  size_t chunk = (sizeof(Entry) + klen + vlen + 8 + 15) & ~size_t{15};  // 8-byte chunk header
  return std::max<size_t>(chunk, 32);
}

Store::Store(const StoreConfig& cfg) : limit_(cfg.mem_limit), seed_(cfg.seed) {
  if (cfg.mem_limit < kMinLimit) throw std::invalid_argument("mem_limit below 64 KiB");
  nbuckets_ = kInitialBuckets;
  mask_ = nbuckets_ - 1;
  buckets_ = static_cast<Entry**>(std::calloc(nbuckets_, sizeof(Entry*)));
  if (!buckets_) throw std::bad_alloc();
  used_ = nbuckets_ * sizeof(Entry*);
}

Store::~Store() {
  for (size_t i = 0; i < nbuckets_; ++i)
    for (Entry* e = buckets_[i]; e;) {
      Entry* n = e->next;
      std::free(e);
      e = n;
    }
  std::free(buckets_);
}

Store::Entry** Store::find(uint64_t h, std::string_view key) const {
  Entry** pp = &buckets_[h & mask_];
  while (*pp) {
    Entry* e = *pp;
    if (e->hash == h && e->klen == key.size() && std::memcmp(e->key(), key.data(), key.size()) == 0) break;
    pp = &e->next;
  }
  return pp;  // *pp == nullptr when absent
}

void Store::release(Entry* e) {
  used_ -= e->cost();
  --count_;
  if (e->expire) --ttl_count_;
  std::free(e);
}

void Store::remove(Entry** pp) {
  Entry* e = *pp;
  *pp = e->next;
  release(e);
}

bool Store::get(std::string_view key, int64_t now, std::string_view& value) {
  Entry** pp = find(hash_bytes(key, seed_), key);
  Entry* e = *pp;
  if (!e) return false;
  if (e->expired(now)) {  // lazy expiry
    remove(pp);
    ++expired_;
    return false;
  }
  e->ref = 1;
  value = std::string_view(e->val(), e->vlen);
  return true;
}

bool Store::del(std::string_view key, int64_t now) {
  Entry** pp = find(hash_bytes(key, seed_), key);
  Entry* e = *pp;
  if (!e) return false;
  bool live = !e->expired(now);
  if (!live) ++expired_;
  remove(pp);
  return live;
}

// CLOCK over buckets: the hand clears reference bits and evicts the first entry whose bit is
// already clear. Expired entries are dropped on sight. Two laps always find a victim if any
// unprotected entry exists. Frees at least one entry per true return.
bool Store::evict_one(const Entry* protect, int64_t now) {
  for (size_t step = 0; step < 2 * nbuckets_; ++step) {
    Entry** pp = &buckets_[hand_];
    hand_ = (hand_ + 1) & mask_;
    while (*pp) {
      Entry* e = *pp;
      if (e == protect) { pp = &e->next; continue; }
      if (e->expired(now)) { remove(pp); ++expired_; return true; }
      if (e->ref) { e->ref = 0; pp = &e->next; continue; }
      remove(pp);
      ++evictions_;
      return true;
    }
  }
  return false;
}

PutResult Store::put(std::string_view key, std::string_view value, int64_t ttl_ms, int64_t now) {
  if (key.size() > UINT16_MAX || value.size() > UINT32_MAX) return PutResult::TooLarge;
  const uint64_t h = hash_bytes(key, seed_);
  Entry** pp = find(h, key);
  Entry* old = *pp;
  if (old && old->expired(now)) {
    remove(pp);
    ++expired_;
    old = nullptr;
  }
  const size_t cost = entry_cost(key.size(), value.size());
  if (cost + nbuckets_ * sizeof(Entry*) > limit_) return PutResult::TooLarge;  // nothing is evicted
  const int64_t expire = ttl_ms > 0 ? now + ttl_ms : 0;
  const size_t old_cost = old ? old->cost() : 0;

  if (old && cost == old_cost) {  // same malloc size class: overwrite in place
    old->vlen = static_cast<uint32_t>(value.size());
    std::memcpy(old->val(), value.data(), value.size());
    ttl_count_ += (expire != 0) - (old->expire != 0);
    old->expire = expire;
    old->ref = 1;
    return PutResult::Replaced;
  }

  while (used_ - old_cost + cost > limit_)
    if (!evict_one(old, now)) return PutResult::NoMemory;
  // Request the whole size class (cost - 8 B chunk header): a later same-class overwrite then
  // stays within the requested size, which sanitizers (and other allocators) require.
  auto* e = static_cast<Entry*>(std::malloc(cost - 8));
  if (!e) return PutResult::NoMemory;
  e->hash = h;
  e->expire = expire;
  e->vlen = static_cast<uint32_t>(value.size());
  e->klen = static_cast<uint16_t>(key.size());
  e->ref = old ? 1 : 0;  // an overwrite counts as an access; a fresh insert does not
  e->pad_ = 0;
  std::memcpy(e->key(), key.data(), key.size());
  std::memcpy(e->val(), value.data(), value.size());

  if (old) {
    pp = find(h, key);  // evictions may have unlinked neighbours: re-locate the link to `old`
    e->next = old->next;
    *pp = e;
    release(old);
  } else {
    Entry*& head = buckets_[h & mask_];
    e->next = head;
    head = e;
  }
  ++count_;
  used_ += cost;
  if (expire) ++ttl_count_;
  maybe_grow();
  return old ? PutResult::Replaced : PutResult::Created;
}

// Stop-the-world rehash (relinks entries, allocates only the new bucket array). Skipped when
// old + new arrays would not fit under the cap, or the table would exceed a quarter of it;
// the table then simply runs at a higher load factor. Never shrinks.
void Store::maybe_grow() {
  if (count_ <= nbuckets_) return;
  const size_t nn = nbuckets_ * 2, nbytes = nn * sizeof(Entry*);
  if (used_ + nbytes > limit_ || nbytes > limit_ / 4) return;
  auto** nb = static_cast<Entry**>(std::calloc(nn, sizeof(Entry*)));
  if (!nb) return;
  for (size_t i = 0; i < nbuckets_; ++i)
    for (Entry* e = buckets_[i]; e;) {
      Entry* next = e->next;
      size_t idx = e->hash & (nn - 1);
      e->next = nb[idx];
      nb[idx] = e;
      e = next;
    }
  std::free(buckets_);
  used_ += nbytes - nbuckets_ * sizeof(Entry*);
  buckets_ = nb;
  nbuckets_ = nn;
  mask_ = nn - 1;
}

size_t Store::sweep(int64_t now, size_t max_buckets) {
  if (ttl_count_ == 0) return 0;
  size_t removed = 0;
  for (size_t i = std::min(max_buckets, nbuckets_); i > 0; --i) {
    Entry** pp = &buckets_[sweep_pos_];
    sweep_pos_ = (sweep_pos_ + 1) & mask_;
    while (*pp) {
      if ((*pp)->expired(now)) { remove(pp); ++expired_; ++removed; }
      else pp = &(*pp)->next;
    }
  }
  return removed;
}

Store::Stats Store::stats() const { return {count_, nbuckets_, used_, limit_, evictions_, expired_}; }

bool Store::check(size_t* max_chain) const {
  size_t n = 0, bytes = nbuckets_ * sizeof(Entry*), ttl = 0, longest = 0;
  for (size_t i = 0; i < nbuckets_; ++i) {
    size_t len = 0;
    for (Entry* e = buckets_[i]; e; e = e->next, ++len) {
      if ((e->hash & mask_) != i || hash_bytes({e->key(), e->klen}, seed_) != e->hash) return false;
      bytes += e->cost();
      ttl += e->expire != 0;
    }
    n += len;
    longest = std::max(longest, len);
  }
  if (max_chain) *max_chain = longest;
  return n == count_ && bytes == used_ && ttl == ttl_count_ && used_ <= limit_;
}

}  // namespace kvx

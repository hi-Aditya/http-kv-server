#pragma once
#include <cstdint>
#include <cstring>
#include <string_view>

namespace kvx {

// Seeded 64-bit multiply-mix hash (wyhash-style). 
inline uint64_t mix64(uint64_t a, uint64_t b) {
  __uint128_t r = static_cast<__uint128_t>(a) * b;
  return static_cast<uint64_t>(r) ^ static_cast<uint64_t>(r >> 64);
}

inline uint64_t hash_bytes(std::string_view s, uint64_t seed) {
  const char* p = s.data();
  size_t n = s.size();
  uint64_t h = seed ^ (n * 0x9E3779B97F4A7C15ull);
  while (n >= 8) {
    uint64_t w;
    std::memcpy(&w, p, 8);
    h = mix64(h ^ w, 0xA0761D6478BD642Full);
    p += 8;
    n -= 8;
  }
  if (n) {
    uint64_t w = 0;
    std::memcpy(&w, p, n);
    h = mix64(h ^ w, 0xE7037ED1A0B428DBull);
  }
  return mix64(h ^ (h >> 29), 0x8EBC6AF09C88C6E3ull);
}

}  // namespace kvx

#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace kvx {

// internals (custom hash table, memory cap, eviction, TTL) without touching callers.
class Store {
 public:
  // Pointer is valid until the next mutating call.
  const std::string* get(std::string_view key) const {
    auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
  }
  bool put(std::string_view key, std::string_view value) {  // true if the key is new
    auto it = map_.find(key);
    if (it != map_.end()) { it->second.assign(value); return false; }
    map_.emplace(std::string(key), std::string(value));
    return true;
  }
  bool del(std::string_view key) {
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    map_.erase(it);
    return true;
  }
  size_t size() const { return map_.size(); }

 private:
  struct Hash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
  };
  std::unordered_map<std::string, std::string, Hash, std::equal_to<>> map_;
};

}  // namespace kvx

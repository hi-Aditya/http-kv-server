#pragma once
#include <list>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

template <typename K=std::string, typename V=std::string>
class LRUCache {
public:
    explicit LRUCache(size_t capacity) : cap_(capacity) {}

    std::optional<V> get(const K& key) {
        std::unique_lock<std::shared_mutex> lock(mu_); // write lock because we mutate order
        auto it = map_.find(key);
        if (it == map_.end()) return std::nullopt;
        order_.splice(order_.begin(), order_, it->second.second);
        return it->second.first;
    }

    void put(const K& key, const V& value) {
        std::unique_lock<std::shared_mutex> lock(mu_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second.first = value;
            order_.splice(order_.begin(), order_, it->second.second);
            return;
        }
        if (order_.size() == cap_) {
            const K& evict_key = order_.back();
            map_.erase(evict_key);
            order_.pop_back();
        }
        order_.push_front(key);
        map_[key] = {value, order_.begin()};
    }

    void erase(const K& key) {
        std::unique_lock<std::shared_mutex> lock(mu_);
        auto it = map_.find(key);
        if (it == map_.end()) return;
        order_.erase(it->second.second);
        map_.erase(it);
    }

    bool contains(const K& key) const {
        std::shared_lock<std::shared_mutex> lock(mu_);
        return map_.count(key) != 0;
    }

private:
    size_t cap_;
    mutable std::shared_mutex mu_;
    std::list<K> order_; // front = MRU
    std::unordered_map<K, std::pair<V, typename std::list<K>::iterator>> map_;
};


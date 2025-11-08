<<<<<<< HEAD
#include "DB.hpp" // your MySQLClient class
=======
#include "DB.hpp"
>>>>>>> 47a2e8c (Updating..)
#include "httplib.h"
#include <iostream>
#include <list>
#include <mutex>
#include <unordered_map>

// =============== LRU Cache Template ===============
template <typename K, typename V> class LRUCache {
  using ListIt = typename std::list<std::pair<K, V>>::iterator;
  std::unordered_map<K, ListIt> map_;
  std::list<std::pair<K, V>> list_;
  size_t capacity_;
  std::mutex mtx_;

public:
  explicit LRUCache(size_t cap) : capacity_(cap) {}

  bool get(const K &key, V &value) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = map_.find(key);
    if (it == map_.end())
      return false;
    list_.splice(list_.begin(), list_, it->second);
    value = it->second->second;
    return true;
  }

  void put(const K &key, const V &value) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = map_.find(key);
    if (it != map_.end()) {
      it->second->second = value;
      list_.splice(list_.begin(), list_, it->second);
      return;
    }
    if (list_.size() == capacity_) {
      auto last = list_.back();
      map_.erase(last.first);
      list_.pop_back();
    }
    list_.emplace_front(key, value);
    map_[key] = list_.begin();
  }

  void remove(const K &key) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = map_.find(key);
    if (it != map_.end()) {
      list_.erase(it->second);
      map_.erase(it);
    }
  }
};

// =============== KV Server ===============
int main() {
  try {
<<<<<<< HEAD
    // ---- 1. Database Config ----
=======
    // ---- Database Config ----
>>>>>>> 47a2e8c (Updating..)
    DBConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 3306;
    cfg.user = "admin";
    cfg.pass = "admin123";
    cfg.db = "KV_store";

    MySQLClient db(cfg);
    LRUCache<int, std::string> cache(100); // cache up to 100 items
    httplib::Server svr;

<<<<<<< HEAD
    std::cout << "✅ Connected to MySQL and cache initialized.\n";

    // ---- 2. Endpoints ----
=======
    std::cout << "Connected to MySQL and cache initialized.\n";

    // ---- Endpoints ----
>>>>>>> 47a2e8c (Updating..)

    // CREATE or UPDATE
    svr.Post("/create",
             [&](const httplib::Request &req, httplib::Response &res) {
               if (!req.has_param("key") || !req.has_param("value")) {
                 res.status = 400;
                 res.set_content("Missing key/value\n", "text/plain");
                 return;
               }
               int key = std::stoi(req.get_param_value("key"));
               std::string value = req.get_param_value("value");

               db.upsert(std::to_string(key), value);
               cache.put(key, value);
               res.set_content("Created/Updated\n", "text/plain");
             });

    // READ
    svr.Get("/read", [&](const httplib::Request &req, httplib::Response &res) {
      if (!req.has_param("key")) {
        res.status = 400;
        res.set_content("Missing key\n", "text/plain");
        return;
      }
      int key = std::stoi(req.get_param_value("key"));
      std::string value;
      if (cache.get(key, value)) {
        res.set_content("[Cache] " + value + "\n", "text/plain");
        return;
      }
      auto dbval = db.read(std::to_string(key));
      if (dbval) {
        cache.put(key, *dbval);
        res.set_content("[DB] " + *dbval + "\n", "text/plain");
      } else {
        res.status = 404;
        res.set_content("Not found\n", "text/plain");
      }
    });

    // UPDATE
    svr.Put("/update",
            [&](const httplib::Request &req, httplib::Response &res) {
              if (!req.has_param("key") || !req.has_param("value")) {
                res.status = 400;
                res.set_content("Missing key/value\n", "text/plain");
                return;
              }
              int key = std::stoi(req.get_param_value("key"));
              std::string value = req.get_param_value("value");
              db.upsert(std::to_string(key), value);
              cache.put(key, value);
              res.set_content("Updated\n", "text/plain");
            });

    // DELETE
    svr.Delete("/delete",
               [&](const httplib::Request &req, httplib::Response &res) {
                 if (!req.has_param("key")) {
                   res.status = 400;
                   res.set_content("Missing key\n", "text/plain");
                   return;
                 }
                 int key = std::stoi(req.get_param_value("key"));
                 if (db.remove(std::to_string(key))) {
                   cache.remove(key);
                   res.set_content("Deleted\n", "text/plain");
                 } else {
                   res.status = 404;
                   res.set_content("Not found\n", "text/plain");
                 }
               });

    // LIST
    svr.Get("/list", [&](const httplib::Request &, httplib::Response &res) {
<<<<<<< HEAD
      MYSQL *conn = db.raw(); // assuming you expose this in MySQLClient
=======
      MYSQL *conn = db.raw();
>>>>>>> 47a2e8c (Updating..)
      if (mysql_query(conn, "SELECT `key`,`value` FROM kv_store") != 0) {
        res.status = 500;
        res.set_content(mysql_error(conn), "text/plain");
        return;
      }
      MYSQL_RES *resset = mysql_store_result(conn);
      MYSQL_ROW row;
      std::ostringstream out;
      out << "---- kv_store ----\n";
      while ((row = mysql_fetch_row(resset)))
        out << row[0] << ": " << row[1] << "\n";
      mysql_free_result(resset);
      res.set_content(out.str(), "text/plain");
    });

    // ---- 3. Start Server ----
    std::cout << "Server running on http://localhost:7000\n";
    svr.listen("0.0.0.0", 7000);

  } catch (const std::exception &ex) {
    std::cerr << "Fatal error: " << ex.what() << "\n";
    return 1;
  }
}

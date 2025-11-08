#include <atomic>
#include <cstdlib>
#include <string>

#include "Config.hpp"
#include "DB.hpp"
#include "LRUCache.hpp"
#include "Log.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

extern std::atomic<bool> g_stop;
extern "C" void handle_sigint(int);

using json = nlohmann::json;

int main() {
  std::signal(SIGINT, handle_sigint);
  std::signal(SIGTERM, handle_sigint);

  // Load config
  Config cfg = Config::load();

  // DB client
  MySQLClient db({.host = cfg.mysql_host,
                  .port = cfg.mysql_port,
                  .user = cfg.mysql_user,
                  .pass = cfg.mysql_pass,
                  .db = cfg.mysql_db});

  // LRU cache
  LRUCache<std::string, std::string> cache(cfg.cache_capacity);

  // HTTP server
  httplib::Server srv;

  // Middlewares: set JSON content type everywhere
  srv.set_default_headers(
      {{"Server", "kvserver/1.0"}, {"Cache-Control", "no-store"}});

  // Health
  srv.Get("/health", [&](const httplib::Request &, httplib::Response &res) {
    res.set_content(R"({"ok":true})", "application/json");
  });

  // CREATE: POST /create  body: {"key":"k", "value":"v"}
  srv.Post("/create", [&](const httplib::Request &req, httplib::Response &res) {
    try {
      auto j = json::parse(req.body);
      std::string key = j.at("key").get<std::string>();
      std::string val = j.at("value").get<std::string>();
      db.upsert(key, val);
      cache.put(key, val);
      res.status = 201;
      res.set_content(R"({"status":"created"})", "application/json");
    } catch (const std::exception &e) {
      kvlog::warn("CREATE error: ", e.what());
      res.status = 400;
      res.set_content(json({{"error", e.what()}}).dump(), "application/json");
    }
  });

  // READ: GET /read?key=k
  srv.Get("/read", [&](const httplib::Request &req, httplib::Response &res) {
    auto key = req.get_param_value("key");
    if (key.empty()) {
      res.status = 400;
      res.set_content(R"({"error":"missing key"})", "application/json");
      return;
    }
    if (auto v = cache.get(key)) {
      res.set_content(
          json({{"key", key}, {"value", *v}, {"cache_hit", true}}).dump(),
          "application/json");
      return;
    }
    if (auto vdb = db.read(key)) {
      cache.put(key, *vdb);
      res.set_content(
          json({{"key", key}, {"value", *vdb}, {"cache_hit", false}}).dump(),
          "application/json");
    } else {
      res.status = 404;
      res.set_content(R"({"error":"not found"})", "application/json");
    }
  });

  // UPDATE: PUT /update  body: {"key":"k", "value":"v"}
  srv.Put("/update", [&](const httplib::Request &req, httplib::Response &res) {
    try {
      auto j = json::parse(req.body);
      std::string key = j.at("key").get<std::string>();
      std::string val = j.at("value").get<std::string>();
      db.upsert(key, val);
      cache.put(key, val);
      res.set_content(R"({"status":"updated"})", "application/json");
    } catch (const std::exception &e) {
      kvlog::warn("UPDATE error: ", e.what());
      res.status = 400;
      res.set_content(json({{"error", e.what()}}).dump(), "application/json");
    }
  });

  // DELETE: DELETE /delete?key=k
  srv.Delete(
      "/delete", [&](const httplib::Request &req, httplib::Response &res) {
        auto key = req.get_param_value("key");
        if (key.empty()) {
          res.status = 400;
          res.set_content(R"({"error":"missing key"})", "application/json");
          return;
        }
        bool ok = db.remove(key);
        cache.erase(key);
        if (ok) {
          res.set_content(R"({"status":"deleted"})", "application/json");
        } else {
          res.status = 404;
          res.set_content(R"({"error":"not found"})", "application/json");
        }
      });

  kvlog::info("Listening on http://", cfg.bind_addr, ":", cfg.port);
  // Blocking call; break via signal
  srv.listen(cfg.bind_addr.c_str(), cfg.port);
  kvlog::info("Server stopped.");
  return 0;
}

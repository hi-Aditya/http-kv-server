#pragma once
#include <cstdlib>
#include <string>
#include <string_view>
#include "Log.hpp"

struct Config {
    std::string bind_addr = "0.0.0.0";
    int port = 6000;

    std::string mysql_host = "127.0.0.1";
    int mysql_port = 3306;
    std::string mysql_user = "kvuser";
    std::string mysql_pass = "kvpass";
    std::string mysql_db   = "kvdb";

    size_t cache_capacity = 100000;

    static std::string env_str(const char* k, std::string def) {
        if (const char* v = std::getenv(k)) return std::string(v);
        return def;
    }
    static int env_int(const char* k, int def) {
        if (const char* v = std::getenv(k)) return std::stoi(v);
        return def;
    }
    static size_t env_size(const char* k, size_t def) {
        if (const char* v = std::getenv(k)) return static_cast<size_t>(std::stoull(v));
        return def;
    }

    static Config load() {
        Config c;
        c.bind_addr     = env_str("BIND_ADDR", c.bind_addr);
        c.port          = env_int("PORT", c.port);
        c.mysql_host    = env_str("MYSQL_HOST", c.mysql_host);
        c.mysql_port    = env_int("MYSQL_PORT", c.mysql_port);
        c.mysql_user    = env_str("MYSQL_USER", c.mysql_user);
        c.mysql_pass    = env_str("MYSQL_PASS", c.mysql_pass);
        c.mysql_db      = env_str("MYSQL_DB",   c.mysql_db);
        c.cache_capacity= env_size("CACHE_CAP", c.cache_capacity);

        kvlog::info("Config: bind=", c.bind_addr, ":", c.port,
                  " mysql=", c.mysql_user, "@", c.mysql_host, ":", c.mysql_port,
                  " db=", c.mysql_db,
                  " cache=", c.cache_capacity);
        return c;
    }
};


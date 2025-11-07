#pragma once
#include <string>
#include <optional>
#include <mysql/mysql.h>

struct DBConfig {
  std::string host = "127.0.0.1";
  int         port = 3306;
  std::string user = "admin";
  std::string pass = "admin123";
  std::string db   = "KV_store";
};

class MySQLClient {
public:
  explicit MySQLClient(const DBConfig &cfg);
  ~MySQLClient();

  void upsert(const std::string &key, const std::string &value);
  std::optional<std::string> read(const std::string &key);
  bool remove(const std::string &key);

  // optional: used by /list endpoint
  MYSQL* raw() const;

private:
  struct Impl;
  Impl* p_;
};


#include "DB.hpp"
#include "Log.hpp"

#include <cstring>       // memset, strlen
#include <mysql/mysql.h> // correct path on Arch (mariadb-libs)
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

<<<<<<< HEAD
// If your headers don't define my_bool (rare), uncomment the next line:
// using my_bool = bool;

=======
>>>>>>> 47a2e8c (Updating..)
struct MySQLClient::Impl {
  MYSQL *conn = nullptr;

  MYSQL_STMT *stmt_upsert = nullptr;
  MYSQL_STMT *stmt_read = nullptr;
  MYSQL_STMT *stmt_delete = nullptr;

<<<<<<< HEAD
  static void bind_string(MYSQL_BIND &b, std::string &s,
                          unsigned long &len) { //,
                                                // my_bool is_null = 0) {
    std::memset(&b, 0, sizeof(b));
    b.buffer_type = MYSQL_TYPE_STRING;
    b.buffer = s.data(); // non-const char* in C++17
=======
  static void bind_string(MYSQL_BIND &b, std::string &s, unsigned long &len) {
    std::memset(&b, 0, sizeof(b));
    b.buffer_type = MYSQL_TYPE_STRING;
    b.buffer = s.data();
>>>>>>> 47a2e8c (Updating..)
    len = static_cast<unsigned long>(s.size());
    b.length = &len;
    b.is_null = nullptr; //&is_null;
  }
};

static std::string make_conn_str(const DBConfig &c) {
  return c.user + "@" + c.host + ":" + std::to_string(c.port) + "/" + c.db;
}

MySQLClient::MySQLClient(const DBConfig &cfg) : p_(new Impl()) {
  p_->conn = mysql_init(nullptr);
  if (!p_->conn)
    throw std::runtime_error("mysql_init failed");

  unsigned int timeout = 5;
  mysql_options(p_->conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
  mysql_options(p_->conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");

  if (!mysql_real_connect(p_->conn, cfg.host.c_str(), cfg.user.c_str(),
                          cfg.pass.c_str(), cfg.db.c_str(),
                          static_cast<unsigned int>(cfg.port), nullptr, 0)) {
    std::string err = mysql_error(p_->conn);
    throw std::runtime_error("mysql_real_connect: " + err);
  }

<<<<<<< HEAD
  // Prepare statements (reused efficiently)
=======
>>>>>>> 47a2e8c (Updating..)
  const char *UPSERT_SQL = "INSERT INTO kv_store (`key`,`value`) VALUES(?,?) "
                           "ON DUPLICATE KEY UPDATE `value`=VALUES(`value`)";
  const char *READ_SQL = "SELECT `value` FROM kv_store WHERE `key`=?";
  const char *DEL_SQL = "DELETE FROM kv_store WHERE `key`=?";

  p_->stmt_upsert = mysql_stmt_init(p_->conn);
  p_->stmt_read = mysql_stmt_init(p_->conn);
  p_->stmt_delete = mysql_stmt_init(p_->conn);

  if (!p_->stmt_upsert || !p_->stmt_read || !p_->stmt_delete)
    throw std::runtime_error("mysql_stmt_init failed");

  if (mysql_stmt_prepare(p_->stmt_upsert, UPSERT_SQL,
                         std::strlen(UPSERT_SQL)) != 0)
    throw std::runtime_error(std::string("stmt_prepare upsert: ") +
                             mysql_stmt_error(p_->stmt_upsert));
  if (mysql_stmt_prepare(p_->stmt_read, READ_SQL, std::strlen(READ_SQL)) != 0)
    throw std::runtime_error(std::string("stmt_prepare read: ") +
                             mysql_stmt_error(p_->stmt_read));
  if (mysql_stmt_prepare(p_->stmt_delete, DEL_SQL, std::strlen(DEL_SQL)) != 0)
    throw std::runtime_error(std::string("stmt_prepare delete: ") +
                             mysql_stmt_error(p_->stmt_delete));

  kvlog::info("Connected to MySQL: ", make_conn_str(cfg));
}

MySQLClient::~MySQLClient() {
  if (!p_)
    return;
  if (p_->stmt_upsert)
    mysql_stmt_close(p_->stmt_upsert);
  if (p_->stmt_read)
    mysql_stmt_close(p_->stmt_read);
  if (p_->stmt_delete)
    mysql_stmt_close(p_->stmt_delete);
  if (p_->conn)
    mysql_close(p_->conn);
  delete p_;
}

void MySQLClient::upsert(const std::string &key, const std::string &value) {
  MYSQL_BIND bind[2];
  unsigned long lens[2];
  std::string k = key, v = value;

  Impl::bind_string(bind[0], k, lens[0]);
  Impl::bind_string(bind[1], v, lens[1]);

  if (mysql_stmt_bind_param(p_->stmt_upsert, bind) != 0)
    throw std::runtime_error(mysql_stmt_error(p_->stmt_upsert));
  if (mysql_stmt_execute(p_->stmt_upsert) != 0)
    throw std::runtime_error(mysql_stmt_error(p_->stmt_upsert));

  mysql_stmt_reset(p_->stmt_upsert);
}

std::optional<std::string> MySQLClient::read(const std::string &key) {
  MYSQL_BIND bind[1];
  unsigned long len0;
  std::string k = key;
  Impl::bind_string(bind[0], k, len0);

  if (mysql_stmt_bind_param(p_->stmt_read, bind) != 0)
    throw std::runtime_error(mysql_stmt_error(p_->stmt_read));
  if (mysql_stmt_execute(p_->stmt_read) != 0)
    throw std::runtime_error(mysql_stmt_error(p_->stmt_read));

  std::vector<char> buf(1024);
  MYSQL_BIND outb[1];
  std::memset(outb, 0, sizeof(outb));
  unsigned long out_len = 0;
  my_bool is_null = 0;
  outb[0].buffer_type = MYSQL_TYPE_STRING;
  outb[0].buffer = buf.data();
  outb[0].buffer_length = buf.size();
  outb[0].length = &out_len;
  outb[0].is_null = &is_null;

  if (mysql_stmt_bind_result(p_->stmt_read, outb) != 0)
    throw std::runtime_error(mysql_stmt_error(p_->stmt_read));
  if (mysql_stmt_store_result(p_->stmt_read) != 0)
    throw std::runtime_error(mysql_stmt_error(p_->stmt_read));

  std::optional<std::string> out;
  int rc = mysql_stmt_fetch(p_->stmt_read);
  if (rc == 0 && !is_null)
    out = std::string(buf.data(), out_len);
  else if (rc == MYSQL_NO_DATA)
    out = std::nullopt;
  else if (rc != 0)
    throw std::runtime_error(mysql_stmt_error(p_->stmt_read));

  mysql_stmt_free_result(p_->stmt_read);
  mysql_stmt_reset(p_->stmt_read);
  return out;
}

bool MySQLClient::remove(const std::string &key) {
  MYSQL_BIND bind[1];
  unsigned long len0;
  std::string k = key;
  Impl::bind_string(bind[0], k, len0);

  if (mysql_stmt_bind_param(p_->stmt_delete, bind) != 0)
    throw std::runtime_error(mysql_stmt_error(p_->stmt_delete));
  if (mysql_stmt_execute(p_->stmt_delete) != 0)
    throw std::runtime_error(mysql_stmt_error(p_->stmt_delete));

  auto affected = mysql_stmt_affected_rows(p_->stmt_delete);
  mysql_stmt_reset(p_->stmt_delete);
  return affected > 0;
}

MYSQL *MySQLClient::raw() const { return p_->conn; }

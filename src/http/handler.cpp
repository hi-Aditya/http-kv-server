#include "http/handler.hpp"

#include <charconv>

namespace kvx::http {
namespace {

constexpr size_t kMaxKey = 256;
constexpr uint64_t kMaxTtlSec = 10ull * 365 * 24 * 3600;

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Percent-decodes `in` into `buf` only when it contains '%'; returns the key view.
bool decode_key(std::string_view in, std::string& buf, std::string_view& key) {
  if (in.find('%') == std::string_view::npos) { key = in; return true; }
  buf.clear();
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '%') { buf += in[i]; continue; }
    if (i + 2 >= in.size()) return false;  // need two hex digits after '%'
    int hi = hexval(in[i + 1]), lo = hexval(in[i + 2]);
    if (hi < 0 || lo < 0) return false;
    buf += static_cast<char>(hi * 16 + lo);
    i += 2;
  }
  key = buf;
  return true;
}

// Query string: only `ttl=<seconds>` (1..10 years) is understood; unknown parameters are ignored.
bool parse_ttl(std::string_view query, int64_t& ttl_ms) {
  ttl_ms = 0;
  while (!query.empty()) {
    size_t amp = query.find('&');
    std::string_view kv = query.substr(0, amp);
    query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
    if (!kv.starts_with("ttl=")) continue;
    kv.remove_prefix(4);
    uint64_t sec = 0;
    auto [p, ec] = std::from_chars(kv.data(), kv.data() + kv.size(), sec);
    if (ec != std::errc{} || p != kv.data() + kv.size() || sec == 0 || sec > kMaxTtlSec) return false;
    ttl_ms = static_cast<int64_t>(sec * 1000);
  }
  return true;
}

}  // namespace

void handle(Store& store, Metrics& metrics, const Request& rq, int64_t now, std::string& out) {
  const bool ka = rq.keep_alive;
  const size_t qpos = rq.target.find('?');
  std::string_view path = rq.target.substr(0, qpos);
  std::string_view query = qpos == std::string_view::npos ? std::string_view{} : rq.target.substr(qpos + 1);

  if (path == "/health") {
    if (rq.method != Method::Get) return append_response(out, 405, "", ka);
    return append_response(out, 200, "ok\n", ka);
  }
  if (path == "/metrics") {
    if (rq.method != Method::Get) return append_response(out, 405, "", ka);
    std::string body;
    metrics.render(body, store.stats());
    return append_response(out, 200, body, ka, "text/plain; version=0.0.4");
  }
  if (!path.starts_with("/kv/")) return append_response(out, 404, "not found\n", ka);

  std::string buf;
  std::string_view key;
  if (!decode_key(path.substr(4), buf, key) || key.empty() || key.size() > kMaxKey)
    return append_response(out, 400, "bad key\n", ka);

  switch (rq.method) {
    case Method::Get:
      if (std::string_view v; store.get(key, now, v))
        return append_response(out, 200, v, ka, "application/octet-stream");
      return append_response(out, 404, "not found\n", ka);
    case Method::Put: {
      int64_t ttl_ms;
      if (!parse_ttl(query, ttl_ms)) return append_response(out, 400, "bad ttl\n", ka);
      switch (store.put(key, rq.body, ttl_ms, now)) {
        case PutResult::Created: return append_response(out, 201, "", ka);
        case PutResult::Replaced: return append_response(out, 200, "", ka);
        case PutResult::TooLarge: return append_response(out, 413, "value exceeds memory limit\n", ka);
        case PutResult::NoMemory: return append_response(out, 503, "out of memory\n", ka);
      }
      return append_response(out, 500, "", ka);
    }
    case Method::Delete:
      return store.del(key, now) ? append_response(out, 204, "", ka)
                                 : append_response(out, 404, "not found\n", ka);
    default:
      return append_response(out, 405, "", ka);
  }
}

}  // namespace kvx::http

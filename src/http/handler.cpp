#include "http/handler.hpp"

namespace kvx::http {
namespace {

constexpr size_t kMaxKey = 256;

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

}  // namespace

void handle(Store& store, Metrics& metrics, const Request& rq, std::string& out) {
  const bool ka = rq.keep_alive;
  std::string_view path = rq.target.substr(0, rq.target.find('?'));

  if (path == "/health") {
    if (rq.method != Method::Get) return append_response(out, 405, "", ka);
    return append_response(out, 200, "ok\n", ka);
  }
  if (path == "/metrics") {
    if (rq.method != Method::Get) return append_response(out, 405, "", ka);
    std::string body;
    metrics.render(body, store.size());
    return append_response(out, 200, body, ka, "text/plain; version=0.0.4");
  }
  if (!path.starts_with("/kv/")) return append_response(out, 404, "not found\n", ka);

  std::string buf;
  std::string_view key;
  if (!decode_key(path.substr(4), buf, key) || key.empty() || key.size() > kMaxKey)
    return append_response(out, 400, "bad key\n", ka);

  switch (rq.method) {
    case Method::Get:
      if (const std::string* v = store.get(key))
        return append_response(out, 200, *v, ka, "application/octet-stream");
      return append_response(out, 404, "not found\n", ka);
    case Method::Put:
      return append_response(out, store.put(key, rq.body) ? 201 : 200, "", ka);
    case Method::Delete:
      return store.del(key) ? append_response(out, 204, "", ka)
                            : append_response(out, 404, "not found\n", ka);
    default:
      return append_response(out, 405, "", ka);
  }
}

}  // namespace kvx::http

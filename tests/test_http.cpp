// Parser unit tests (pure logic, no sockets). Plain asserts that survive NDEBUG.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "http/http.hpp"

using namespace kvx::http;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static ParseState st(const std::string& s) { return parse(s).state; }

int main() {
  const std::string get = "GET /kv/a HTTP/1.1\r\nHost: x\r\n\r\n";
  const std::string put = "PUT /kv/a HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";

  // Every strict prefix of a valid request is "need more"; the full request is Done.
  for (const std::string& req : {get, put}) {
    for (size_t n = 0; n < req.size(); ++n) CHECK(st(req.substr(0, n)) == ParseState::Need);
    ParseResult r = parse(req);
    CHECK(r.state == ParseState::Done && r.consumed == req.size());
  }
  ParseResult r = parse(put);
  CHECK(r.req.method == Method::Put && r.req.target == "/kv/a" && r.req.body == "hello" && r.req.keep_alive);

  // Pipelining: consumed covers exactly the first request.
  r = parse(get + put);
  CHECK(r.state == ParseState::Done && r.consumed == get.size() && r.req.method == Method::Get);

  // Keep-alive semantics.
  CHECK(!parse("GET / HTTP/1.0\r\n\r\n").req.keep_alive);
  CHECK(parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n").req.keep_alive);
  CHECK(!parse("GET / HTTP/1.1\r\nconnection: Close\r\n\r\n").req.keep_alive);
  CHECK(!parse("GET / HTTP/1.1\r\nConnection: keep-alive, close\r\n\r\n").req.keep_alive);

  // Header names are case-insensitive; values are trimmed.
  r = parse("PUT /k HTTP/1.1\r\ncontent-LENGTH:   2  \r\n\r\nhi");
  CHECK(r.state == ParseState::Done && r.req.body == "hi");

  // Rejections.
  CHECK(st("GET /a HTTP/2.0\r\n\r\n") == ParseState::Bad);
  CHECK(st("GET a HTTP/1.1\r\n\r\n") == ParseState::Bad);          // target must start with '/'
  CHECK(st("GET /a\r\n\r\n") == ParseState::Bad);                  // no version
  CHECK(st("GET /a b HTTP/1.1\r\n\r\n") == ParseState::Bad);
  CHECK(st("GET /a HTTP/1.1\r\nBadHeader\r\n\r\n") == ParseState::Bad);
  CHECK(st("GET /a HTTP/1.1\r\nName : v\r\n\r\n") == ParseState::Bad);  // space before colon
  CHECK(st("GET /a HTTP/1.1\r\n folded: v\r\n\r\n") == ParseState::Bad);
  CHECK(st("PUT /a HTTP/1.1\r\nContent-Length: 1x\r\n\r\nx") == ParseState::Bad);
  CHECK(st("PUT /a HTTP/1.1\r\nContent-Length: -1\r\n\r\n") == ParseState::Bad);
  CHECK(st("PUT /a HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nxx") == ParseState::Bad);
  CHECK(st("PUT /a HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nxx") == ParseState::Done);
  CHECK(st("PUT /a HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") == ParseState::NotImplemented);

  // Limits: oversized body is rejected from the header alone (no buffering needed).
  CHECK(st("PUT /a HTTP/1.1\r\nContent-Length: " + std::to_string(kMaxBody) + "\r\n\r\n") == ParseState::Need);
  CHECK(st("PUT /a HTTP/1.1\r\nContent-Length: " + std::to_string(kMaxBody + 1) + "\r\n\r\n") == ParseState::BodyTooLarge);
  CHECK(st("PUT /a HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n") == ParseState::BodyTooLarge);
  CHECK(st("GET /a HTTP/1.1\r\nX: " + std::string(kMaxHeader, 'a')) == ParseState::HeaderTooLarge);   // no terminator yet
  CHECK(st("GET /a HTTP/1.1\r\nX: " + std::string(kMaxHeader, 'a') + "\r\n\r\n") == ParseState::HeaderTooLarge);

  std::printf(g_fail ? "%d FAILED\n" : "all http parser tests passed\n", g_fail);
  return g_fail ? 1 : 0;
}

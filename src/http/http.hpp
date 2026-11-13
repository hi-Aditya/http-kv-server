#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace kvx::http {

inline constexpr size_t kMaxHeader = 8 * 1024;  // request line + headers, incl. final CRLFCRLF
inline constexpr size_t kMaxBody = 1024 * 1024;

enum class Method { Get, Put, Delete, Other };

struct Request {
  Method method = Method::Other;
  std::string_view target;  // views point into the caller's buffer
  std::string_view body;
  bool keep_alive = true;
};

enum class ParseState { Need, Done, Bad, HeaderTooLarge, BodyTooLarge, NotImplemented };

struct ParseResult {
  ParseState state = ParseState::Need;
  size_t consumed = 0;  // valid when Done
  Request req;
};

// Incremental: call again with more data after Need. Any non-Need/Done state is
// fatal for the connection. Need is only returned while buf.size() < kMaxHeader
// + kMaxBody, so a buffer of that size always suffices.
ParseResult parse(std::string_view buf);

int status_of(ParseState s);  // HTTP status for a fatal parse state

void append_response(std::string& out, int status, std::string_view body, bool keep_alive,
                     std::string_view content_type = "text/plain");

}  // namespace kvx::http

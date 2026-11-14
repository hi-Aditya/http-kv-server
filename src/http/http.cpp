#include "http/http.hpp"

#include <cctype>
#include <charconv>

namespace kvx::http {
namespace {

using std::string_view;
constexpr auto npos = string_view::npos;

bool ieq(string_view a, string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

string_view trim(string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

}  // namespace

ParseResult parse(string_view buf) {
  ParseResult r;
  size_t hend = buf.find("\r\n\r\n");
  if (hend == npos) {
    if (buf.size() > kMaxHeader) r.state = ParseState::HeaderTooLarge;
    return r;  // Need
  }
  if (hend + 4 > kMaxHeader) { r.state = ParseState::HeaderTooLarge; return r; }

  string_view head = buf.substr(0, hend);
  size_t eol = head.find("\r\n");
  string_view line = head.substr(0, eol);
  string_view rest = eol == npos ? string_view{} : head.substr(eol + 2);

  // Request line: METHOD SP target SP HTTP/1.x
  size_t sp1 = line.find(' '), sp2 = sp1 == npos ? npos : line.find(' ', sp1 + 1);
  if (sp2 == npos) { r.state = ParseState::Bad; return r; }
  string_view method = line.substr(0, sp1), version = line.substr(sp2 + 1);
  r.req.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  bool v11 = version == "HTTP/1.1";
  if ((!v11 && version != "HTTP/1.0") || r.req.target.empty() || r.req.target[0] != '/') {
    r.state = ParseState::Bad;
    return r;
  }
  for (char ch : r.req.target)
    if (static_cast<unsigned char>(ch) <= 0x20 || ch == 0x7f) { r.state = ParseState::Bad; return r; }
  r.req.method = method == "GET" ? Method::Get : method == "PUT" ? Method::Put
               : method == "DELETE" ? Method::Delete : Method::Other;
  r.req.keep_alive = v11;

  // Headers
  size_t content_length = 0;
  bool have_cl = false, close_seen = false, ka_seen = false;
  while (!rest.empty()) {
    size_t e = rest.find("\r\n");
    string_view h = rest.substr(0, e);
    rest = e == npos ? string_view{} : rest.substr(e + 2);
    size_t colon = h.find(':');
    if (colon == npos || colon == 0) { r.state = ParseState::Bad; return r; }
    string_view name = h.substr(0, colon), value = trim(h.substr(colon + 1));
    if (name.find_first_of(" \t") != npos) { r.state = ParseState::Bad; return r; }

    if (ieq(name, "content-length")) {
      size_t v = 0;
      auto [p, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
      if (value.empty() || ec == std::errc::result_out_of_range) {
        r.state = value.empty() ? ParseState::Bad : ParseState::BodyTooLarge;
        return r;
      }
      if (ec != std::errc{} || p != value.data() + value.size()) { r.state = ParseState::Bad; return r; }
      if (have_cl && v != content_length) { r.state = ParseState::Bad; return r; }
      have_cl = true;
      content_length = v;
    } else if (ieq(name, "transfer-encoding")) {
      r.state = ParseState::NotImplemented;
      return r;
    } else if (ieq(name, "connection")) {
      while (!value.empty()) {  // comma-separated tokens
        size_t c = value.find(',');
        string_view tok = trim(value.substr(0, c));
        if (ieq(tok, "close")) close_seen = true;
        else if (ieq(tok, "keep-alive")) ka_seen = true;
        value = c == npos ? string_view{} : value.substr(c + 1);
      }
    }
  }
  if (close_seen) r.req.keep_alive = false;
  else if (ka_seen) r.req.keep_alive = true;
  if (content_length > kMaxBody) { r.state = ParseState::BodyTooLarge; return r; }

  size_t total = hend + 4 + content_length;  // <= kMaxHeader + kMaxBody, no overflow
  if (buf.size() < total) return r;           // Need
  r.req.body = buf.substr(hend + 4, content_length);
  r.consumed = total;
  r.state = ParseState::Done;
  return r;
}

int status_of(ParseState s) {
  switch (s) {
    case ParseState::HeaderTooLarge: return 431;
    case ParseState::BodyTooLarge: return 413;
    case ParseState::NotImplemented: return 501;
    default: return 400;
  }
}

void append_response(std::string& out, int status, std::string_view body, bool keep_alive,
                     std::string_view content_type) {
  const char* reason = "Error";
  switch (status) {
    case 200: reason = "OK"; break;
    case 201: reason = "Created"; break;
    case 204: reason = "No Content"; break;
    case 400: reason = "Bad Request"; break;
    case 404: reason = "Not Found"; break;
    case 405: reason = "Method Not Allowed"; break;
    case 413: reason = "Content Too Large"; break;
    case 431: reason = "Request Header Fields Too Large"; break;
    case 501: reason = "Not Implemented"; break;
    case 503: reason = "Service Unavailable"; break;
    default: break;
  }
  out.reserve(out.size() + body.size() + 128);
  out += "HTTP/1.1 ";
  out += std::to_string(status);
  out += ' ';
  out += reason;
  out += "\r\nContent-Length: ";
  out += std::to_string(body.size());
  if (!body.empty()) { out += "\r\nContent-Type: "; out += content_type; }
  out += keep_alive ? "\r\n\r\n" : "\r\nConnection: close\r\n\r\n";
  out += body;
}

}  // namespace kvx::http

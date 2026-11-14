#pragma once
#include <cstdint>
#include <string>

namespace kvx {

struct Metrics {
  uint64_t requests = 0, bad_requests = 0;
  uint64_t conns_accepted = 0, conns_rejected = 0, conns_idle_closed = 0;
  uint64_t conns_active = 0;  // gauge, maintained by the event loop

  void render(std::string& out, uint64_t keys) const {
    auto line = [&](const char* name, const char* type, uint64_t v) {
      out += "# TYPE "; out += name; out += ' '; out += type; out += '\n';
      out += name; out += ' '; out += std::to_string(v); out += '\n';
    };
    line("kvx_requests_total", "counter", requests);
    line("kvx_bad_requests_total", "counter", bad_requests);
    line("kvx_connections_accepted_total", "counter", conns_accepted);
    line("kvx_connections_rejected_total", "counter", conns_rejected);
    line("kvx_connections_idle_closed_total", "counter", conns_idle_closed);
    line("kvx_connections_active", "gauge", conns_active);
    line("kvx_keys", "gauge", keys);
  }
};

}  // namespace kvx

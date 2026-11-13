#pragma once
#include <string>

#include "http/http.hpp"
#include "metrics.hpp"
#include "store/store.hpp"

namespace kvx::http {

// Routes one parsed request and appends the full response to `out`. `wall_ms` is unix time.
void handle(Store& store, Metrics& metrics, const Request& req, int64_t wall_ms, std::string& out);

}  // namespace kvx::http

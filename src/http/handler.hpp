#pragma once
#include <string>

#include "http/http.hpp"
#include "metrics.hpp"
#include "store/store.hpp"

namespace kvx::http {

// Routes one parsed request and appends the full response to `out`.
void handle(Store& store, Metrics& metrics, const Request& req, std::string& out);

}  // namespace kvx::http

#pragma once
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace kvlog {
inline std::mutex& mtx() { static std::mutex m; return m; }

inline std::string ts() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto tt = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.'
        << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

template <typename... Args>
inline void info(Args&&... args) {
    std::lock_guard<std::mutex> lock(mtx());
    std::cerr << "[" << ts() << "][INFO] ";
    (std::cerr << ... << args) << '\n';
}

template <typename... Args>
inline void warn(Args&&... args) {
    std::lock_guard<std::mutex> lock(mtx());
    std::cerr << "[" << ts() << "][WARN] ";
    (std::cerr << ... << args) << '\n';
}

template <typename... Args>
inline void error(Args&&... args) {
    std::lock_guard<std::mutex> lock(mtx());
    std::cerr << "[" << ts() << "][ERROR] ";
    (std::cerr << ... << args) << '\n';
}
} // namespace log


#include <malloc.h>
#include <signal.h>
#include <sys/random.h>
#include <sys/resource.h>

#include <charconv>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "net/event_loop.hpp"
#include "util/log.hpp"

namespace {

int g_stop_fd = -1;

extern "C" void on_signal(int) {
  uint64_t one = 1;
  ssize_t r = write(g_stop_fd, &one, sizeof one);  // async-signal-safe; result irrelevant
  (void)r;
}

template <class T>
bool parse_num(const char* s, T& out) {
  std::string_view v(s);
  auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
  return ec == std::errc{} && p == v.data() + v.size();
}

int usage() {
  std::fputs("usage: kvx [--bind IPV4] [--port N] [--max-conns N] [--idle-ms N] [--mem-mb N]\n", stderr);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  kvx::Options opt;
  size_t mem_mb = 256;
  for (int i = 1; i < argc; i += 2) {
    std::string_view k = argv[i];
    if (i + 1 >= argc) return usage();
    bool ok = false;
    if (k == "--bind") { opt.bind = argv[i + 1]; ok = true; }
    else if (k == "--port") ok = parse_num(argv[i + 1], opt.port);
    else if (k == "--max-conns") ok = parse_num(argv[i + 1], opt.max_conns) && opt.max_conns > 0;
    else if (k == "--mem-mb") ok = parse_num(argv[i + 1], mem_mb) && mem_mb >= 1 && mem_mb <= (1u << 20);
    else if (k == "--idle-ms") ok = parse_num(argv[i + 1], opt.idle_ms) && opt.idle_ms > 0;
    if (!ok) return usage();
  }

  // Fixed mmap threshold: big values and buffers are returned to
  // the OS on free, which keeps RSS close to the accounted memory under churn.
  mallopt(M_MMAP_THRESHOLD, 128 * 1024);

  // Every connection costs one fd; raise the soft limit to the hard limit if needed.
  rlimit rl{};
  const rlim_t need = opt.max_conns + 32;
  if (getrlimit(RLIMIT_NOFILE, &rl) < 0) { KVX_WARN("getrlimit: %s", std::strerror(errno)); return 1; }
  if (rl.rlim_cur < need) {
    rl.rlim_cur = need;
    if (rl.rlim_cur > rl.rlim_max || setrlimit(RLIMIT_NOFILE, &rl) < 0) {
      KVX_WARN("--max-conns %zu needs RLIMIT_NOFILE >= %lu", opt.max_conns, static_cast<unsigned long>(need));
      return 1;
    }
  }

  try {
    kvx::StoreConfig sc;
    sc.mem_limit = mem_mb << 20;
    if (getrandom(&sc.seed, sizeof sc.seed, 0) != static_cast<ssize_t>(sizeof sc.seed)) {
      KVX_WARN("getrandom failed");
      return 1;
    }
    kvx::Store store(sc);
    kvx::Metrics metrics;
    kvx::EventLoop loop(opt, store, metrics);
    g_stop_fd = loop.stop_fd();

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, nullptr) < 0 || sigaction(SIGTERM, &sa, nullptr) < 0) {
      KVX_WARN("sigaction: %s", std::strerror(errno));
      return 1;
    }
    signal(SIGPIPE, SIG_IGN);  // sends also use MSG_NOSIGNAL

    std::printf("listening %s:%u\n", opt.bind.c_str(), loop.port());
    std::fflush(stdout);
    loop.run();
    KVX_INFO("shutdown: %lu requests served", static_cast<unsigned long>(metrics.requests));
  } catch (const std::exception& e) {
    KVX_WARN("fatal: %s", e.what());
    return 1;
  }
  return 0;
}

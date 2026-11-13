#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "http/handler.hpp"
#include "util/fd.hpp"

namespace kvx {

struct Options {
  std::string bind = "127.0.0.1";  // IPv4 only
  uint16_t port = 8080;            // 0 = ephemeral (see port())
  size_t max_conns = 4096;         // beyond this: 503 + close
  int idle_ms = 60000;             // no read/write progress for this long -> close
};

// Single-threaded, level-triggered epoll server. Why level-triggered: reads are
// deliberately paused for backpressure (stalled readers, full output buffer), and
// toggling EPOLLIN is the whole mechanism; with edge-triggered we'd have to track
// undrained sockets by hand to avoid lost wake-ups. One recv() per ready connection
// per iteration also bounds per-wakeup work (fairness).
class EventLoop {
 public:
  EventLoop(const Options& opt, Store& store, Metrics& metrics);  // throws std::system_error
  ~EventLoop();
  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  uint16_t port() const { return port_; }
  int stop_fd() const { return stop_.get(); }  // write 8 bytes (uint64 1) to stop; async-signal-safe
  void run();                                  // returns after stop; closes all connections

 private:
  struct Conn;
  void accept_ready();
  void reject(int fd);
  void on_conn_event(Conn& c, uint32_t events);
  bool read_some(Conn& c);  // false on fatal socket error
  bool process(Conn& c);  // true if it consumed at least one request
  void finish(Conn& c);  // process + flush, close if done, update epoll interest
  bool flush(Conn& c);   // false on fatal socket error
  void close_conn(Conn& c);
  void sweep_idle();

  Options opt_;
  Store& store_;
  Metrics& metrics_;
  Fd listen_, ep_, stop_, spare_;  // spare_: reserve fd for shedding on EMFILE
  uint16_t port_ = 0;
  int64_t now_ = 0;
  bool stopping_ = false;
  std::vector<std::unique_ptr<Conn>> conns_;
};

}  // namespace kvx

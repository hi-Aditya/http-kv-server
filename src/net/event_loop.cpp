#include "net/event_loop.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <ctime>

#include "util/log.hpp"

namespace kvx {
namespace {

constexpr size_t kReadCap = http::kMaxHeader + http::kMaxBody;  // max rbuf
constexpr size_t kWriteHigh = 256 * 1024;  // stop parsing new requests while this much is unsent
constexpr size_t kShrinkAbove = 64 * 1024;
char g_listen_tag, g_stop_tag;  // epoll data.ptr sentinels

int64_t clock_ms(clockid_t id) {
  timespec ts;
  clock_gettime(id, &ts);
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void ep_ctl(int ep, int op, int fd, uint32_t ev, void* ptr) {
  epoll_event e{};
  e.events = ev;
  e.data.ptr = ptr;
  if (epoll_ctl(ep, op, fd, &e) < 0) throw_errno("epoll_ctl");
}

}  // namespace

struct EventLoop::Conn {
  int fd;
  size_t idx = 0;  // slot in conns_
  std::string rbuf;
  size_t rpos = 0, rlen = 0;  // unparsed bytes are rbuf[rpos, rlen)
  std::string wbuf;
  size_t woff = 0;  // unsent bytes are wbuf[woff, size)
  bool close_after = false, eof = false;
  uint32_t ev = EPOLLIN;  // registered interest
  int64_t last_ms = 0;
  explicit Conn(int f) : fd(f) {}
  ~Conn() { ::close(fd); }
  size_t pending() const { return wbuf.size() - woff; }
};

EventLoop::EventLoop(const Options& opt, Store& store, Metrics& metrics)
    : opt_(opt), store_(store), metrics_(metrics) {
  listen_ = own(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0), "socket");
  int one = 1;
  if (setsockopt(listen_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) throw_errno("SO_REUSEADDR");
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(opt.port);
  if (inet_pton(AF_INET, opt.bind.c_str(), &addr.sin_addr) != 1)
    throw std::runtime_error("bad bind address: " + opt.bind);
  if (bind(listen_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) throw_errno("bind");
  if (listen(listen_.get(), SOMAXCONN) < 0) throw_errno("listen");
  socklen_t len = sizeof addr;
  if (getsockname(listen_.get(), reinterpret_cast<sockaddr*>(&addr), &len) < 0) throw_errno("getsockname");
  port_ = ntohs(addr.sin_port);

  ep_ = own(epoll_create1(EPOLL_CLOEXEC), "epoll_create1");
  stop_ = own(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC), "eventfd");
  spare_ = own(open("/dev/null", O_RDONLY | O_CLOEXEC), "open /dev/null");
  ep_ctl(ep_.get(), EPOLL_CTL_ADD, listen_.get(), EPOLLIN, &g_listen_tag);
  ep_ctl(ep_.get(), EPOLL_CTL_ADD, stop_.get(), EPOLLIN, &g_stop_tag);
}

EventLoop::~EventLoop() = default;

void EventLoop::run() {
  std::array<epoll_event, 256> evs;
  const int tick = std::clamp(opt_.idle_ms / 2, 10, 100);        // epoll timeout
  const int idle_every = std::clamp(opt_.idle_ms / 2, 10, 1000);  // idle-connection sweep period
  int64_t last_idle = now_ = clock_ms(CLOCK_MONOTONIC), last_ttl = now_;
  wall_ = clock_ms(CLOCK_REALTIME);
  while (!stopping_) {
    int n = epoll_wait(ep_.get(), evs.data(), static_cast<int>(evs.size()), tick);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw_errno("epoll_wait");
    }
    now_ = clock_ms(CLOCK_MONOTONIC);
    wall_ = clock_ms(CLOCK_REALTIME);
    for (int i = 0; i < n; ++i) {
      void* p = evs[static_cast<size_t>(i)].data.ptr;
      uint32_t events = evs[static_cast<size_t>(i)].events;
      if (p == &g_listen_tag) accept_ready();
      else if (p == &g_stop_tag) stopping_ = true;
      else on_conn_event(*static_cast<Conn*>(p), events);  // each conn appears once per batch
    }
    if (now_ - last_idle >= idle_every) { sweep_idle(); last_idle = now_; }
    // Active expiry, rate-limited by the monotonic clock so a wall-clock jump cannot stall it:
    // at most 1024 buckets per 10 ms (~1% of one core on a cold table).
    if (now_ - last_ttl >= 10) { store_.sweep(wall_, 1024); last_ttl = now_; }
  }
  conns_.clear();
  metrics_.conns_active = 0;
}

void EventLoop::reject(int fd) {
  std::string resp;
  http::append_response(resp, 503, "too many connections\n", false);
  (void)send(fd, resp.data(), resp.size(), MSG_NOSIGNAL | MSG_DONTWAIT);  // best effort
  ::close(fd);
  ++metrics_.conns_rejected;
}

void EventLoop::accept_ready() {
  for (int i = 0; i < 64; ++i) {  // bounded per wakeup
    int fd = accept4(listen_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      if (errno == EINTR || errno == ECONNABORTED) continue;
      if (errno == EMFILE || errno == ENFILE) {  // shed one using the reserve fd, else we'd spin
        spare_.reset();
        int s = accept4(listen_.get(), nullptr, nullptr, SOCK_CLOEXEC);
        if (s >= 0) reject(s);
        spare_ = Fd(open("/dev/null", O_RDONLY | O_CLOEXEC));
        KVX_WARN("accept: out of file descriptors");
        return;
      }
      KVX_WARN("accept: %s", std::strerror(errno));
      return;
    }
    if (conns_.size() >= opt_.max_conns) { reject(fd); continue; }
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);  // latency hint only
    auto c = std::make_unique<Conn>(fd);  // owns fd from here
    c->last_ms = now_;
    try {
      ep_ctl(ep_.get(), EPOLL_CTL_ADD, fd, EPOLLIN, c.get());
    } catch (const std::system_error& e) {
      KVX_WARN("epoll add: %s", e.what());
      continue;
    }
    c->idx = conns_.size();
    conns_.push_back(std::move(c));
    ++metrics_.conns_accepted;
    metrics_.conns_active = conns_.size();
  }
}

// Every path that calls close_conn() must return immediately: it destroys `c`.
void EventLoop::on_conn_event(Conn& c, uint32_t events) {
  if (events & EPOLLERR) return close_conn(c);
  if (events & EPOLLOUT) {
    if (!flush(c)) return close_conn(c);
  }
  if (events & (EPOLLIN | EPOLLHUP)) {
    if (!read_some(c)) return close_conn(c);
  }
  finish(c);
}

// Returns false on a fatal socket error. EOF is recorded, not fatal (half-close:
// the peer may still be waiting for responses).
bool EventLoop::read_some(Conn& c) {
  if (c.eof || c.close_after) return true;
  if (c.rlen == c.rbuf.size()) {
    if (c.rbuf.size() >= kReadCap) return false;  // unreachable: parser bounds Need; defensive
    c.rbuf.resize(std::min(kReadCap, std::max<size_t>(4096, c.rbuf.size() * 2)));
  }
  ssize_t n = recv(c.fd, c.rbuf.data() + c.rlen, c.rbuf.size() - c.rlen, 0);
  if (n > 0) {
    c.rlen += static_cast<size_t>(n);
    c.last_ms = now_;
  } else if (n == 0) {
    c.eof = true;
  } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
    return false;
  }
  return true;
}

// Parse and answer as many buffered requests as output backpressure allows.
bool EventLoop::process(Conn& c) {
  bool progressed = false;
  while (!c.close_after && c.rpos < c.rlen && c.pending() < kWriteHigh) {
    http::ParseResult r = http::parse({c.rbuf.data() + c.rpos, c.rlen - c.rpos});
    if (r.state == http::ParseState::Need) break;
    if (r.state == http::ParseState::Done) {
      ++metrics_.requests;
      http::handle(store_, metrics_, r.req, wall_, c.wbuf);
      c.rpos += r.consumed;
      if (!r.req.keep_alive) c.close_after = true;
      progressed = true;
    } else {
      ++metrics_.bad_requests;
      http::append_response(c.wbuf, http::status_of(r.state), "", false);
      c.close_after = true;
      progressed = true;
    }
  }
  // Nothing more can be parsed and the peer is done sending: finish after flushing.
  if (c.eof && c.pending() < kWriteHigh) c.close_after = true;
  if (c.rpos == c.rlen) {
    c.rpos = c.rlen = 0;
    if (c.rbuf.size() > kShrinkAbove) std::string().swap(c.rbuf);
  } else if (c.rpos > 0) {
    std::memmove(c.rbuf.data(), c.rbuf.data() + c.rpos, c.rlen - c.rpos);
    c.rlen -= c.rpos;
    c.rpos = 0;
  }
  return progressed;
}

bool EventLoop::flush(Conn& c) {
  while (c.pending() > 0) {
    ssize_t n = send(c.fd, c.wbuf.data() + c.woff, c.pending(), MSG_NOSIGNAL);
    if (n > 0) {
      c.woff += static_cast<size_t>(n);
      c.last_ms = now_;
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    } else {
      return false;
    }
  }
  if (c.pending() == 0) {
    c.wbuf.clear();
    c.woff = 0;
    if (c.wbuf.capacity() > kShrinkAbove) std::string().swap(c.wbuf);
  }
  return true;
}

void EventLoop::finish(Conn& c) {
  // Alternate parse and flush: an immediate flush may drain output below the high-water
  // mark with no EPOLLOUT to follow, so leftover pipelined requests must be resumed here.
  for (;;) {
    bool progressed = process(c);
    if (!flush(c)) return close_conn(c);
    if (!progressed || c.pending() >= kWriteHigh) break;
  }
  if (c.pending() == 0 && c.close_after) return close_conn(c);
  // Backpressure: stop reading while output is backed up or the connection is winding down.
  uint32_t want = 0;
  if (!c.eof && !c.close_after && c.pending() < kWriteHigh) want |= EPOLLIN;
  if (c.pending() > 0) want |= EPOLLOUT;
  if (want != c.ev) {
    epoll_event e{};
    e.events = want;
    e.data.ptr = &c;
    if (epoll_ctl(ep_.get(), EPOLL_CTL_MOD, c.fd, &e) < 0) return close_conn(c);
    c.ev = want;
  }
}

void EventLoop::close_conn(Conn& c) {  // closing the fd also removes it from the epoll set
  Conn* last = conns_.back().get();
  conns_[c.idx].swap(conns_.back());
  last->idx = c.idx;
  conns_.pop_back();  // destroys c (closes fd)
  metrics_.conns_active = conns_.size();
}

void EventLoop::sweep_idle() {
  for (size_t i = conns_.size(); i-- > 0;) {  // backwards: swap-remove only moves visited slots
    if (now_ - conns_[i]->last_ms > opt_.idle_ms) {
      ++metrics_.conns_idle_closed;
      close_conn(*conns_[i]);
    }
  }
}

}  // namespace kvx

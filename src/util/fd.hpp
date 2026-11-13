#pragma once
#include <cerrno>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace kvx {

[[noreturn]] inline void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

// Owning file descriptor (close on destruction, move-only).
class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) : fd_(fd) {}
  Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
  Fd& operator=(Fd&& o) noexcept {
    if (this != &o) reset(std::exchange(o.fd_, -1));
    return *this;
  }
  ~Fd() { reset(); }
  int get() const { return fd_; }
  void reset(int fd = -1) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

// Wrap a syscall result: throws on -1 with errno, otherwise owns the fd.
inline Fd own(int fd, const char* what) {
  if (fd < 0) throw_errno(what);
  return Fd(fd);
}

}  // namespace kvx

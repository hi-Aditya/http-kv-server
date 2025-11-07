#include "Log.hpp"
#include <atomic>
#include <csignal>

std::atomic<bool> g_stop{false};

extern "C" void handle_sigint(int) {
  kvlog::warn("SIGINT received, shutting down...");
  g_stop = true;
}

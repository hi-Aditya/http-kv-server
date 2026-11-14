#pragma once
#include <cstdio>

#define KVX_LOG(level, ...)                 \
  do {                                      \
    std::fprintf(stderr, level " " __VA_ARGS__); \
    std::fputc('\n', stderr);               \
  } while (0)
#define KVX_INFO(...) KVX_LOG("INFO", __VA_ARGS__)
#define KVX_WARN(...) KVX_LOG("WARN", __VA_ARGS__)

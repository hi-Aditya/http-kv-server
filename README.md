# KVX

A persistent, concurrent HTTP key-value server in C++20 (epoll, sharded store, per-shard WAL).
Work in progress, built milestone by milestone; see `DECISIONS.md`. Current state: **M1** (event loop + HTTP + in-memory store).

## Build and test
```bash
cmake --preset release && cmake --build --preset release
ctest --preset release                      # parser unit tests + functional tests
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
scripts/loc.sh                              # source-size budget check
```

## Run
```bash
build/release/kvx --port 8080 [--bind 127.0.0.1] [--max-conns 4096] [--idle-ms 60000]
curl -X PUT --data-binary hello localhost:8080/kv/greeting   # 201 new / 200 overwrite
curl localhost:8080/kv/greeting                              # 200 value / 404
curl -X DELETE localhost:8080/kv/greeting                    # 204 / 404
curl localhost:8080/health ; curl localhost:8080/metrics
```
Keys are percent-decoded, 1-256 bytes. Values up to 1 MiB. No chunked requests (501).

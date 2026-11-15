# KVX

A persistent, concurrent HTTP key-value server in C++20 (epoll, sharded store, per-shard WAL).
Work in progress, built milestone by milestone; see `DECISIONS.md`. Current state: **M2** (event loop + HTTP + capped store with CLOCK eviction and TTL).

## Build and test
```bash
cmake --preset release && cmake --build --preset release
ctest --preset release                      # parser unit tests + functional tests
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
scripts/loc.sh                              # source-size budget check
```

## Run
```bash
build/release/kvx --port 8080 [--bind 127.0.0.1] [--max-conns 4096] [--idle-ms 60000] [--mem-mb 256]
curl -X PUT --data-binary hello localhost:8080/kv/greeting   # 201 new / 200 overwrite
curl localhost:8080/kv/greeting                              # 200 value / 404
curl -X DELETE localhost:8080/kv/greeting                    # 204 / 404
curl -X PUT --data-binary tmp 'localhost:8080/kv/session?ttl=30'  # expires after 30 s
curl localhost:8080/health ; curl localhost:8080/metrics
```
Keys are percent-decoded, 1-256 bytes. Values up to 1 MiB (413 if larger than the memory cap allows). No chunked requests (501).
When `--mem-mb` is reached, older entries are evicted (CLOCK); see ADR-4/5/6 in `DECISIONS.md` for what the cap does and does not count.

## Benchmarks
```bash
cmake --build --preset release --target bench_store
taskset -c 2 build/release/bench_store 1000000 64 5 | tee bench/results/store.csv
```

# Decisions

**ADR-1: Fresh start in this repo.** The original MySQL-backed prototype (httplib + MariaDB + `std::list`/`unordered_map` LRU under one mutex) is replaced entirely; none of it fits the shared-nothing, WAL-based design. It remains in git history (`47a2e8c`).

**ADR-2: Level-triggered epoll.** Backpressure works by removing EPOLLIN while a connection's output is backed up or it is winding down; level-triggered makes that a one-line interest change, whereas edge-triggered needs per-socket "still has unread data" bookkeeping to avoid lost wake-ups. One `recv` per ready connection per iteration bounds per-wakeup work. Revisit only if profiling shows `epoll_ctl`/wake-up overhead matters.

**ADR-3: Bounded buffers instead of queues.** Per connection: read buffer <= 8 KiB + 1 MiB (the largest request the parser accepts), write buffer <= 256 KiB high-water mark + one response. Above the mark no further pipelined requests are parsed and EPOLLIN is dropped, so TCP flow control pushes back on the client. Connections beyond `--max-conns` get 503 and are closed; connections making no read/write progress for `--idle-ms` are closed.

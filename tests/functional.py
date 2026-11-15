#!/usr/bin/env python3
"""KVX M1 functional tests: semantics, pipelining, 1k connections, conn cap, slow reader.
Usage: functional.py /path/to/kvx   (spawns its own server instances on ephemeral ports)"""
import os, re, resource, signal, socket, subprocess, sys, time

BIN = sys.argv[1]
MAX_BODY = 1024 * 1024
failures = []

def check(cond, msg):
    if not cond:
        failures.append(msg)
        print("FAIL:", msg)

class Server:
    def __init__(self, *args):
        self.p = subprocess.Popen([BIN, "--port", "0", *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        m = re.match(r"listening (\S+):(\d+)", self.p.stdout.readline())
        assert m, "server did not start: " + self.p.stderr.read()
        self.port = int(m.group(2))
    def conn(self, rcvbuf=None):
        s = socket.socket()
        if rcvbuf: s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        s.settimeout(10)
        s.connect(("127.0.0.1", self.port))
        return s
    def rss_kb(self):
        return int(re.search(r"VmRSS:\s+(\d+)", open(f"/proc/{self.p.pid}/status").read()).group(1))
    def metric(self, name):
        s = self.conn(); body = request(s, "GET", "/metrics")[2].decode(); s.close()
        return int(re.search(rf"^{name} (\d+)$", body, re.M).group(1))
    def stop(self):
        self.p.send_signal(signal.SIGTERM)
        try: rc = self.p.wait(10)
        except subprocess.TimeoutExpired: self.p.kill(); rc = -1
        check(rc == 0, f"clean shutdown on SIGTERM (rc={rc})")

def recv_response(s, buf=b""):
    """Read one response; returns (status, headers, body, leftover)."""
    while b"\r\n\r\n" not in buf:
        d = s.recv(65536)
        if not d: raise EOFError(buf)
        buf += d
    head, rest = buf.split(b"\r\n\r\n", 1)
    lines = head.decode().split("\r\n")
    status = int(lines[0].split()[1])
    hdr = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines[1:]}
    n = int(hdr.get("content-length", 0))
    while len(rest) < n:
        d = s.recv(1 << 20)
        if not d: raise EOFError
        rest += d
    return status, hdr, rest[:n], rest[n:]

def req_bytes(method, path, body=b"", extra=""):
    return f"{method} {path} HTTP/1.1\r\nHost: t\r\nContent-Length: {len(body)}\r\n{extra}\r\n".encode() + body

def request(s, method, path, body=b""):
    s.sendall(req_bytes(method, path, body))
    st, h, b, left = recv_response(s)
    assert not left
    return st, h, b

def closed_by_peer(s):
    try: return s.recv(1) == b""
    except (ConnectionResetError, socket.timeout): return True

def test_semantics(srv):
    s = srv.conn()
    check(request(s, "GET", "/health")[0:1] == (200,), "health 200")
    check(request(s, "PUT", "/kv/a", b"1")[0] == 201, "PUT new -> 201")
    check(request(s, "PUT", "/kv/a", b"22")[0] == 200, "PUT overwrite -> 200")
    st, h, b = request(s, "GET", "/kv/a")
    check((st, b) == (200, b"22"), "GET returns last value")
    check(request(s, "GET", "/kv/missing")[0] == 404, "GET missing -> 404")
    check(request(s, "DELETE", "/kv/a")[0] == 204, "DELETE existing -> 204")
    check(request(s, "DELETE", "/kv/a")[0] == 404, "DELETE again -> 404")
    check(request(s, "GET", "/kv/a")[0] == 404, "GET after delete -> 404")
    check(request(s, "PUT", "/kv/empty", b"")[0] == 201 and request(s, "GET", "/kv/empty")[2] == b"", "empty value roundtrip")
    blob = bytes(range(256)) * 4
    request(s, "PUT", "/kv/bin", blob)
    check(request(s, "GET", "/kv/bin")[2] == blob, "binary value roundtrip")
    request(s, "PUT", "/kv/sp%20ace%2Fx", b"v")
    check(request(s, "GET", "/kv/sp%20ace%2fx")[2] == b"v", "percent-decoded keys")
    check(request(s, "GET", "/kv/%zz")[0] == 400, "bad percent escape -> 400")
    check(request(s, "GET", "/kv/")[0] == 400, "empty key -> 400")
    check(request(s, "GET", "/kv/" + "k" * 257)[0] == 400, "key > 256 -> 400")
    check(request(s, "PUT", "/kv/" + "k" * 256, b"x")[0] == 201, "key == 256 ok")
    check(request(s, "POST", "/kv/a")[0] == 405, "POST -> 405")
    check(request(s, "GET", "/nope")[0] == 404, "unknown path -> 404")
    big = os.urandom(MAX_BODY)
    check(request(s, "PUT", "/kv/big", big)[0] == 201 and request(s, "GET", "/kv/big")[2] == big, "max-size value roundtrip")
    s.close()
    s = srv.conn(); s.sendall(req_bytes("PUT", "/kv/toobig", b"")[:-0 or None].replace(b"Content-Length: 0", f"Content-Length: {MAX_BODY + 1}".encode()))
    st, _, _, _ = recv_response(s)
    check(st == 413 and closed_by_peer(s), "oversized body -> 413 + close")
    check(request(srv.conn(), "GET", "/kv/toobig")[0] == 404, "oversized value not stored")

def test_protocol(srv):
    s = srv.conn(); s.sendall(b"GARBAGE\r\n\r\n")
    check(recv_response(s)[0] == 400 and closed_by_peer(s), "garbage -> 400 + close")
    s = srv.conn(); s.sendall(b"PUT /kv/c HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n")
    check(recv_response(s)[0] == 501 and closed_by_peer(s), "chunked -> 501 + close")
    s = srv.conn(); s.sendall(b"GET /" + b"a" * 9000 + b" HTTP/1.1\r\n\r\n")
    check(recv_response(s)[0] == 431 and closed_by_peer(s), "huge header -> 431 + close")
    s = srv.conn(); s.sendall(req_bytes("GET", "/health", extra="Connection: close\r\n"))
    check(recv_response(s)[0] == 200 and closed_by_peer(s), "Connection: close honoured")
    # request dribbled one byte at a time
    s = srv.conn()
    for b in req_bytes("PUT", "/kv/drip", b"xyz"): s.sendall(bytes([b])); time.sleep(0.0005)
    check(recv_response(s)[0] == 201 and request(s, "GET", "/kv/drip")[2] == b"xyz", "byte-at-a-time request")
    # pipelining: 200 requests in one write, responses in order
    s = srv.conn()
    s.sendall(b"".join(req_bytes("PUT", f"/kv/p{i}", str(i).encode()) + req_bytes("GET", f"/kv/p{i}") for i in range(100)))
    buf, ok = b"", True
    for i in range(100):
        st, _, _, buf = recv_response(s, buf); ok &= st == 201
        st, _, b, buf = recv_response(s, buf); ok &= (st, b) == (200, str(i).encode())
    check(ok, "pipelined responses in order")
    # half-close: client sends then shuts down write side, still gets the response
    s = srv.conn(); s.sendall(req_bytes("GET", "/health")); s.shutdown(socket.SHUT_WR)
    check(recv_response(s)[0] == 200 and closed_by_peer(s), "half-close still answered, then closed")

def test_1k_connections(srv):
    lim = resource.getrlimit(resource.RLIMIT_NOFILE)[1]
    resource.setrlimit(resource.RLIMIT_NOFILE, (min(lim, 4096), lim))
    n = 1000
    socks = [srv.conn() for _ in range(n)]
    for i, s in enumerate(socks): s.sendall(req_bytes("PUT", f"/kv/c{i}", f"v{i}".encode()))
    ok = all(recv_response(s)[0] == 201 for s in socks)
    for i, s in enumerate(socks): s.sendall(req_bytes("GET", f"/kv/c{(i * 7) % n}"))
    ok &= all(recv_response(s)[2] == f"v{(i * 7) % n}".encode() for i, s in enumerate(socks))
    check(ok, "1000 concurrent connections: all PUT/GET correct")
    check(srv.metric("kvx_connections_active") >= n, "active connections gauge >= 1000")
    for s in socks: s.close()
    time.sleep(0.3)
    check(srv.metric("kvx_connections_active") <= 2, "connections released after close")

def test_conn_cap():
    srv = Server("--max-conns", "8")
    held = [srv.conn() for _ in range(8)]
    for s in held: request(s, "GET", "/health")
    extra = srv.conn()
    check(recv_response(extra)[0] == 503 and closed_by_peer(extra), "connection beyond cap -> 503 + close")
    held.pop().close(); time.sleep(0.2)
    check(request(srv.conn(), "GET", "/health")[0] == 200, "slot reusable after close")
    check(srv.metric("kvx_connections_rejected_total") >= 1, "rejected counter")
    srv.stop()

def test_slow_reader():
    srv = Server("--idle-ms", "2000")
    s0 = srv.conn(); value = os.urandom(512 * 1024); request(s0, "PUT", "/kv/v", value)
    base_rss = srv.rss_kb(); base_reqs = srv.metric("kvx_requests_total")
    N = 400
    slow = srv.conn(rcvbuf=4096)
    slow.sendall(req_bytes("GET", "/kv/v") * N)   # ~200 MB of responses wanted, none read
    time.sleep(1.0)
    handled = srv.metric("kvx_requests_total") - base_reqs - 1  # -1: the /metrics request itself
    growth_mb = (srv.rss_kb() - base_rss) / 1024
    check(handled < 100, f"backpressure: only {handled}/{N} pipelined requests served while reader stalled")
    check(growth_mb < 16, f"RSS growth {growth_mb:.1f} MB while reader stalled (bound 16 MB)")
    check(request(s0, "GET", "/health")[0] == 200, "other clients unaffected by stalled reader")
    print(f"  slow reader: served {handled}/{N} while stalled, RSS growth {growth_mb:.1f} MB")
    # a reader that resumes gets every response, intact and in order
    s1 = srv.conn(); s1.sendall(req_bytes("GET", "/kv/v") * 50)
    buf, ok = b"", True
    for _ in range(50):
        st, _, b, buf = recv_response(s1, buf); ok &= st == 200 and b == value
    check(ok, "resumed reader receives all pipelined responses intact")
    # the stalled connection makes no progress: idle timeout reclaims it
    time.sleep(3.5)
    check(srv.metric("kvx_connections_idle_closed_total") >= 1, "stalled connection closed by idle timeout")
    dead = False
    try:
        while slow.recv(1 << 20): pass
        dead = True
    except (ConnectionResetError, socket.timeout): dead = True
    check(dead, "stalled client sees the connection closed")
    srv.stop()


def metrics(srv):
    s = srv.conn(); body = request(s, "GET", "/metrics")[2].decode(); s.close()
    return {m.group(1): int(m.group(2)) for m in re.finditer(r"^(kvx_\w+) (\d+)$", body, re.M)}

def test_ttl():
    srv = Server()
    s = srv.conn()
    for bad in ("ttl=0", "ttl=abc", "ttl=-1", "ttl=1.5", "ttl=", "ttl=99999999999"):
        check(request(s, "PUT", f"/kv/b?{bad}", b"x")[0] == 400, f"bad {bad} -> 400")
    check(request(s, "GET", "/kv/b")[0] == 404, "rejected ttl PUT stored nothing")
    check(request(s, "PUT", "/kv/u?foo=1", b"x")[0] == 201, "unknown query parameter ignored")
    check(request(s, "PUT", "/kv/e?ttl=1", b"v")[0] == 201 and request(s, "GET", "/kv/e")[2] == b"v", "ttl key readable")
    request(s, "PUT", "/kv/keep?ttl=1", b"a"); request(s, "PUT", "/kv/keep", b"b")   # plain overwrite clears ttl
    request(s, "PUT", "/kv/ext?ttl=1", b"a"); request(s, "PUT", "/kv/ext?ttl=60", b"b")
    base = metrics(srv)["kvx_keys"]
    check(flood(srv, [(f"sw{i}?ttl=1", b"v") for i in range(2000)]), "2000 ttl puts accepted")
    time.sleep(1.5)
    check(request(s, "GET", "/kv/e")[0] == 404, "ttl key gone after expiry (lazy)")
    check(request(s, "GET", "/kv/keep")[2] == b"b" and request(s, "GET", "/kv/ext")[2] == b"b", "overwrite replaces ttl")
    m = metrics(srv)   # nothing read sw*: the active sweep must have reclaimed them
    check(m["kvx_keys"] <= base, f"active sweep reclaimed unread ttl keys (keys={m['kvx_keys']}, base={base})")
    check(m["kvx_expired_total"] >= 2000, f"expired counter {m['kvx_expired_total']}")
    srv.stop()

def test_mem_limit_small():
    srv = Server("--mem-mb", "1")
    s = srv.conn()
    check(metrics(srv)["kvx_memory_limit_bytes"] == 1 << 20, "limit reported")
    check(request(s, "PUT", "/kv/huge", b"x" * MAX_BODY)[0] == 413, "1 MiB value under 1 MiB cap -> 413")
    check(request(s, "PUT", "/kv/h1", b"a" * 400000)[0] == 201 and request(s, "PUT", "/kv/h2", b"b" * 400000)[0] == 201, "two 400 KB values fit")
    check(request(s, "PUT", "/kv/h3", b"c" * 400000)[0] == 201, "third value evicts instead of failing")
    m = metrics(srv)
    check(m["kvx_evictions_total"] >= 1 and m["kvx_memory_used_bytes"] <= m["kvx_memory_limit_bytes"], "evicted, cap respected")
    check(request(s, "GET", "/kv/h3")[2] == b"c" * 400000, "newest value present")
    srv.stop()

def flood(srv, items, batch=200):
    s = srv.conn(); ok = True
    for i in range(0, len(items), batch):
        chunk = items[i:i + batch]
        s.sendall(b"".join(req_bytes("PUT", f"/kv/{k}", v) for k, v in chunk))
        buf = b""
        for _ in chunk:
            st, _, _, buf = recv_response(s, buf); ok &= st in (200, 201)
    s.close()
    return ok

def test_write_flood():
    import random
    cap_mb = 16
    srv = Server("--mem-mb", str(cap_mb))
    cap = cap_mb << 20
    # Stated overhead: RSS <= 2x cap + 12 MB (allocator fragmentation under size-churn, connection
    # buffers, binary). Not asserted under sanitizers: ASan's quarantine inflates RSS.
    bound_kb = cap * 2 // 1024 + 12 * 1024
    rng = random.Random(1)
    filler = b"x" * (1 << 20)
    def phase(name, items):
        check(flood(srv, items), f"{name}: all PUTs accepted")
        m, rss = metrics(srv), srv.rss_kb()
        check(m["kvx_memory_used_bytes"] <= cap, f"{name}: accounted memory {m['kvx_memory_used_bytes']} <= cap")
        check(m["kvx_evictions_total"] > 0, f"{name}: evictions happened")
        if not os.environ.get("KVX_SANITIZED"):
            check(rss <= bound_kb, f"{name}: RSS {rss/1024:.1f} MB <= bound {bound_kb/1024:.1f} MB")
        print(f"  {name}: RSS {rss/1024:.1f} MB, accounted {m['kvx_memory_used_bytes']/1048576:.1f} / {cap_mb} MB, "
              f"keys {m['kvx_keys']}, evictions {m['kvx_evictions_total']}")
        return m
    base = srv.rss_kb()
    phase("1KB flood (60k puts, ~60 MB)", [(f"f{i}", filler[:1024]) for i in range(60000)])
    sizes = [int(2 ** rng.uniform(4, 16)) for _ in range(6000)] + [rng.randrange(128 << 10, MAX_BODY) for _ in range(40)]
    rng.shuffle(sizes)
    phase(f"mixed 16B-1MB flood ({sum(sizes) >> 20} MB)", [(f"m{i}", filler[:n]) for i, n in enumerate(sizes)])
    s = srv.conn()
    check(request(s, "GET", f"/kv/m{len(sizes) - 1}")[2] == filler[:sizes[-1]], "most recent value intact after flood")
    print(f"  baseline RSS before flood: {base/1024:.1f} MB")
    srv.stop()

def main():
    srv = Server("--idle-ms", "5000")
    for t in (test_semantics, test_protocol, test_1k_connections):
        print("==", t.__name__); t(srv)
    srv.stop()
    for t in (test_ttl, test_mem_limit_small, test_write_flood, test_conn_cap, test_slow_reader):
        print("==", t.__name__); t()
    print("FAILED" if failures else "all functional tests passed")
    sys.exit(1 if failures else 0)

main()

# Project: **NovaCache** — A Distributed In-Memory Data Store (a "build your own Redis")

Here's a project that reads as a genuine piece of infrastructure — the kind of thing companies actually run in production — while forcing you to touch nearly every corner of C++: raw sockets, custom parsers, concurrency, memory management, templates, RAII, design patterns, persistence, and testing. It's also a great GitHub portfolio piece because it's instantly understandable ("I built a Redis clone") and demoable (`redis-cli`-style client hitting your server).

---

## Why this project, specifically

A lot of "learn C++" projects (calculators, to-do apps, tic-tac-toe) don't force you into the language's harder corners. A key-value store does, almost by necessity:


| C++ Concept                      | Where it shows up in NovaCache                                                                                  |
| -------------------------------- | --------------------------------------------------------------------------------------------------------------- |
| RAII & smart pointers            | Connection lifetime, buffer management, resource cleanup on socket close                                        |
| Templates & generics             | Generic command registry, typed value wrappers, generic serialization                                           |
| STL mastery                      | `unordered_map`, custom hash tables (build your own too, for comparison), `deque`, `variant`                    |
| Concurrency                      | Thread pool for connections, mutexes/shared_mutex for the data store, condition variables, atomics for stats    |
| Networking                       | Raw POSIX sockets + `epoll`/`select` event loop (or `io_uring` as a stretch goal)                               |
| Memory management                | Custom allocator for small objects, manual buffer pooling to avoid churn                                        |
| Design patterns                  | Command pattern (dispatch table), Singleton (server instance), Observer (pub/sub), Strategy (eviction policies) |
| Exception handling & error codes | Protocol errors, `std::expected`/`std::optional` style APIs                                                     |
| Serialization/parsing            | Hand-written RESP (REdis Serialization Protocol)-like parser                                                    |
| Persistence & I/O                | Write-ahead log (WAL), snapshotting to disk, binary file formats                                                |
| Modern C++ (17/20)               | `std::variant`, `std::optional`, structured bindings, `std::string_view`, concepts (C++20)                      |
| Testing & tooling                | GoogleTest/Catch2, CMake, CI via GitHub Actions, sanitizers (ASan/TSan/UBSan)                                   |


---

## Architecture Overview

```
                     ┌─────────────────────────────┐
   TCP Clients  ───▶ │        Event Loop            │
  (your CLI, or      │  (epoll-based reactor)        │
   real redis-cli)   └──────────────┬───────────────┘
                                     │ dispatches parsed commands
                                     ▼
                     ┌─────────────────────────────┐
                     │      Command Dispatcher       │  ← Command pattern
                     │  (GET/SET/DEL/EXPIRE/LPUSH…)  │
                     └──────────────┬───────────────┘
                                     ▼
                     ┌─────────────────────────────┐
                     │      Core Data Store          │
                     │  (sharded hash map + locks)   │
                     └──────┬─────────────┬─────────┘
                            ▼             ▼
                   ┌────────────┐  ┌───────────────┐
                   │  WAL / AOF  │  │  Pub/Sub Bus   │
                   │ (durability)│  │ (Observer ptrn)│
                   └────────────┘  └───────────────┘

```

---

## Phased Build Plan (each phase is a usable milestone — great for commit history / README progress checklist)

### Phase 1 — Core Engine (the "make it work" phase)

- A TCP server using raw sockets (`socket`, `bind`, `listen`, `accept`) on Linux (or WinSock if you want Windows support too).
- A blocking, single-threaded loop first — get correctness before concurrency.
- In-memory store: `std::unordered_map<std::string, Value>` where `Value` is a `std::variant<std::string, int64_t, std::vector<std::string>, std::unordered_set<std::string>>` to support strings, integers, lists, and sets.
- Commands: `PING`, `SET`, `GET`, `DEL`, `EXISTS`, `EXPIRE`, `TTL`, `KEYS`.
- A hand-rolled parser for a RESP-like text protocol so you practice string parsing, not just calling a library.

### Phase 2 — Concurrency

- Replace the single-threaded loop with an `epoll`-based reactor (event-driven I/O multiplexing) so one thread can juggle thousands of connections.
- Add a thread pool for CPU-bound command execution, decoupled from the I/O thread.
- Shard the data store (e.g., 16 shards, each with its own `std::shared_mutex`) to reduce lock contention — a classic real-world concurrency technique.
- Add atomic counters for live stats (`INFO` command: connections, ops/sec, memory usage).

### Phase 3 — Persistence

- **Write-Ahead Log (AOF-style):** every mutating command is appended to a log file before being applied, so the store can recover after a crash by replaying it.
- **Snapshotting (RDB-style):** periodically serialize the whole store to a compact binary file (custom binary format — great practice with `ofstream`, endianness, and struct packing).
- Startup logic: load snapshot, then replay WAL entries since the snapshot.

### Phase 4 — Advanced Data Structures & Commands

- Lists (`LPUSH`, `RPUSH`, `LRANGE`) backed by `std::deque`.
- Sets (`SADD`, `SMEMBERS`, `SINTER`).
- Sorted sets (`ZADD`, `ZRANGE`) — implement a **skip list** yourself instead of using a library. This is the single best "shows you actually understand data structures" feature you can add.
- LRU eviction policy when a configurable memory cap is hit (Strategy pattern: pluggable eviction policies — LRU, LFU, random).

### Phase 5 — Pub/Sub & Replication (the "distributed" part)

- `SUBSCRIBE` / `PUBLISH` — Observer pattern, fan-out to subscribed connections.
- A minimal **leader-replica replication**: the leader streams its WAL to one or more replica processes over a socket, and replicas apply it live. This alone teaches you a huge amount about consistency, networking, and process coordination.

### Phase 6 — Polish & "Production" Touches

- A CLI client (`novacache-cli`) that mimics `redis-cli`, built with the same protocol parser (reuse your code — good practice in modularity).
- Config file support (port, max memory, persistence interval) via a simple INI/TOML-lite parser.
- Structured logging (levels, timestamps).
- Optional: basic auth (`AUTH` command with a password check).
- Optional: TLS via OpenSSL for encrypted client connections.
- Benchmark tool measuring ops/sec (comparable to `redis-benchmark`) — great for a "Performance" section in your README with real numbers.

---

## Suggested Repository Structure

```
novacache/
├── CMakeLists.txt
├── README.md                 (with architecture diagram, quickstart, benchmarks)
├── include/
│   ├── server/
│   ├── store/
│   ├── protocol/
│   ├── persistence/
│   └── util/
├── src/
│   └── (mirrors include/)
├── cli/
│   └── novacache-cli.cpp
├── tests/
│   ├── unit/          (GoogleTest: parser, store, skip list, eviction)
│   └── integration/   (spins up server, drives it with real socket clients)
├── benchmarks/
├── docs/
│   └── PROTOCOL.md     (spec of your wire protocol — makes it look legit)
└── .github/workflows/ci.yml   (build + test + sanitizers on push)

```

---

## Testing Strategy (this is what separates "assignment" from "engineering project")

- **Unit tests** for the parser, the store, the skip list, and eviction policies.
- **Integration tests** that open real sockets and issue commands against a running server instance — validates the whole stack.
- **Concurrency tests** using ThreadSanitizer to catch data races in your sharded store.
- **Fuzz testing** the protocol parser with malformed input (AFL or libFuzzer) — this is a fantastic, resume-worthy addition since it shows security-mindedness.

---

## Stretch Goals (pick 1–2 to make it stand out further)

- **Lua-style scripting**: implement a tiny expression/scripting language for server-side atomic operations (like Redis's `EVAL`) — huge template-metaprogramming and parsing practice.
- **Consistent hashing** to shard data across multiple NovaCache nodes (real distributed systems territory).
- **HTTP admin dashboard** (a small embedded HTTP server serving a stats page) — shows you can build two protocols in one project.
- **Cluster mode**: gossip-based node discovery between multiple NovaCache instances.

---

## What makes this "involving and interesting" rather than dry

- You get a runnable, demoable thing on day one (a server you can `telnet` into).
- Every phase adds a visibly new capability, so your commit history tells a story.
- It's a domain (databases/caching) that's universally recognized by anyone reviewing your GitHub — recruiters and engineers immediately know what they're looking at.
- It naturally scales in difficulty — you can stop after Phase 3 and still have something impressive, or go all the way to Phase 6 for something exceptional.

---

Want me to scaffold the actual repo — CMake setup, folder structure, a working Phase 1 TCP echo server, and a starter README — so you have a real first commit to push tonight?
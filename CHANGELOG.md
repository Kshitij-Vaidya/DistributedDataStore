# Changelog

All notable changes to NovaCache will be documented in this file. The format is
based on Keep a Changelog, and releases follow Semantic Versioning.

## [Unreleased]

### Added

- C++20 CMake project and reusable `novacache_core` library.
- Server, CLI, benchmark, and GoogleTest targets.
- Debug, Release, ASan/UBSan, and TSan presets.
- Strict project warning, formatting, and static-analysis policies.
- Initial protocol, architecture, development, and configuration contracts.
- Linux and macOS continuous integration.
- Binary-safe RESP2 value, incremental parser, and encoder implementation.
- Sharded in-memory store with injectable clocks, lazy expiration, and
  absolute wall-clock expiry stamps for future persistence.
- Registry-driven `PING`, `ECHO`, `SET`, `GET`, `DEL`, `EXISTS`, `EXPIRE`,
  `TTL`, and `KEYS` commands.
- RAII POSIX sockets, blocking TCP server, and one-shot CLI compatible with the
  implemented `redis-cli` command subset.
- Unit and real-socket integration tests for protocol boundaries, command
  errors, pipelining, fragmentation, TTLs, and server shutdown.
- Portable epoll/kqueue reactor with edge-triggered I/O and wakeup support.
- Bounded worker pool, per-connection command strands, and ordered responses.
- Per-shard `shared_mutex` locking, active expiry sampling, and `INFO` stats.
- Concurrent client/pipeline integration tests and store concurrency tests.

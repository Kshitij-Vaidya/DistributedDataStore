# NovaCache

NovaCache is a C++20 distributed in-memory data store inspired by Redis. The
project is being built in independently testable phases to explore RESP2
parsing, POSIX networking, portable event loops, concurrency, persistence,
advanced data structures, pub/sub, and leader-replica replication.

> **Current status: Phase 0 foundation.** The library, application targets,
> tests, sanitizer presets, documentation, and CI are present. The executable
> stubs only support `--version`; they do not accept network connections yet.

## Planned architecture

```text
TCP clients -> epoll/kqueue reactor -> RESP2 parser -> command dispatcher
                                                     |
                                                worker pool
                                                     |
                          sharded store <-> WAL/snapshots -> replicas
                                                     |
                                                   pub/sub
```

Linux uses epoll; macOS uses kqueue behind one reactor interface. See
[the architecture specification](docs/ARCHITECTURE.md) for ownership and
ordering rules.

## Requirements

- CMake 3.24+
- Ninja
- A C++20 compiler (Apple Clang, Clang, or GCC)
- Git and network access during first configuration

## Build and test

```bash
cmake --preset debug
cmake --build --preset debug -j
ctest --preset debug --output-on-failure
./scripts/smoke-test.sh debug
```

Verify the Phase 0 programs:

```bash
./build/debug/novacache-server --version
./build/debug/novacache-cli --version
./build/debug/novacache-benchmark --version
```

Running an executable without `--version` intentionally fails until its
functionality is implemented.

Other supported presets are:

```bash
cmake --preset release
cmake --preset asan
cmake --preset tsan
```

See [the development guide](docs/DEVELOPMENT.md) for platform setup,
sanitizers, formatting, and troubleshooting.

## Protocol scope

NovaCache targets a practical RESP2 subset usable by `redis-cli`. Initial
commands will be `PING`, `ECHO`, `SET`, `GET`, `DEL`, `EXISTS`, `EXPIRE`,
`TTL`, and `KEYS`. This is not a claim of complete Redis compatibility.
Wire types, errors, limits, and TTL semantics are specified in
[docs/PROTOCOL.md](docs/PROTOCOL.md).

## Roadmap

- [x] Phase 0: specifications, CMake foundation, quality tooling, and CI
- [ ] Phase 1: RESP2, synchronous server, core store, commands, and TTL
- [ ] Phase 2: epoll/kqueue reactor, thread pool, sharding, and statistics
- [ ] Phase 3: WAL, snapshots, and crash recovery
- [ ] Phase 4: lists, sets, sorted sets, memory limits, and eviction
- [ ] Phase 5: pub/sub and leader-replica replication
- [ ] Phase 6: complete CLI, configuration, logging, auth, and benchmarks
- [ ] Phase 7: fuzzing, failure injection, containers, and release hardening

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Protocol](docs/PROTOCOL.md)
- [Development](docs/DEVELOPMENT.md)
- [Project brief](problem_statement.md)

## License

NovaCache is available under the [MIT License](LICENSE).
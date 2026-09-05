# NovaCache

NovaCache is a C++20 distributed in-memory data store inspired by Redis. The
project is being built in independently testable phases to explore RESP2
parsing, POSIX networking, portable event loops, concurrency, persistence,
advanced data structures, pub/sub, and leader-replica replication.

> **Current status: Phase 3 persistence.** NovaCache serves many RESP2 clients
> through a portable epoll/kqueue reactor, a bounded worker pool with
> per-connection command strands, and a sharded store. Optional WAL + snapshot
> durability recovers acknowledged writes after restart or crash under
> `fsync=always`.

## Architecture

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

Start the server and issue commands:

```bash
./build/debug/novacache-server --host 127.0.0.1 --port 6379 --workers 4 \
  --data-dir /tmp/novacache-data --fsync everysec --snapshot-interval 300

./build/debug/novacache-cli -p 6379 PING
./build/debug/novacache-cli -p 6379 SET demo value
./build/debug/novacache-cli -p 6379 GET demo
./build/debug/novacache-cli -p 6379 INFO
redis-cli -h 127.0.0.1 -p 6379 PING
```

Omit `--data-dir` to run without persistence. Durability modes and on-disk
formats are documented in [docs/PERSISTENCE.md](docs/PERSISTENCE.md).

`novacache-cli` is a one-shot client; interactive mode and the benchmark tool
are deferred to Phase 6.

Other supported presets:

```bash
cmake --preset release
cmake --preset asan
cmake --preset tsan
```

See [the development guide](docs/DEVELOPMENT.md) for platform setup,
sanitizers, formatting, and troubleshooting.

## Protocol scope

NovaCache targets a practical RESP2 subset usable by `redis-cli`. Supported
commands are `PING`, `ECHO`, `SET`, `GET`, `DEL`, `EXISTS`, `EXPIRE`, `TTL`,
`KEYS`, and `INFO`. This is not a claim of complete Redis compatibility. Wire
types, errors, limits, and TTL semantics are specified in
[docs/PROTOCOL.md](docs/PROTOCOL.md).

## Roadmap

- [x] Phase 0: specifications, CMake foundation, quality tooling, and CI
- [x] Phase 1: RESP2, synchronous server, core store, commands, and TTL
- [x] Phase 2: epoll/kqueue reactor, thread pool, sharding, and statistics
- [x] Phase 3: WAL, snapshots, and crash recovery
- [ ] Phase 4: lists, sets, sorted sets, memory limits, and eviction
- [ ] Phase 5: pub/sub and leader-replica replication
- [ ] Phase 6: complete CLI, configuration, logging, auth, and benchmarks
- [ ] Phase 7: fuzzing, failure injection, containers, and release hardening

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Protocol](docs/PROTOCOL.md)
- [Persistence](docs/PERSISTENCE.md)
- [Development](docs/DEVELOPMENT.md)
- [Project brief](docs/PROBLEMSTATEMENT.md)

## License

NovaCache is available under the [MIT License](LICENSE).

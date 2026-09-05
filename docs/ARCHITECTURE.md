# NovaCache Architecture

## Current state

Phase 3 builds on the Phase 2 concurrent server with optional durability: a
versioned write-ahead log, atomic snapshots, fsync modes (`always` /
`everysec` / `none`), and recovery that loads a snapshot then replays later WAL
records. Persistence stays off unless `--data-dir` is configured.

## Component flow

```text
RESP2 client
    |
portable reactor (epoll on Linux, kqueue on macOS)
    |
connection buffers -> incremental RESP2 parser -> command registry
                                                |
                                           worker pool
                                                |
                +-------------------------------+------------------+
                |                               |                  |
          sharded store                    WAL/snapshot         pub/sub
                                                |
                                         replica stream
```

The shared `novacache_core` library contains protocol, store, command, socket,
persistence, and server behavior. Thin programs under `apps/` provide the
server, CLI, and benchmark entry points.
Public interfaces live under `include/novacache/`; implementation details live
under `src/`.

## Ownership and concurrency rules

1. A constructed `Server` owns its runtime resources; there is no global
   singleton.
2. RAII objects own file descriptors, threads, buffers, and persistence files.
3. The reactor exclusively owns socket registration and socket writes.
4. Workers execute immutable parsed commands and enqueue completions; they do
   not write directly to sockets.
5. Per-connection sequence numbers preserve pipelined response order.
6. Connection generation identifiers prevent late work from targeting a
   recycled file descriptor.
7. A key maps to one fixed shard. Multi-shard locks are acquired in ascending
   shard-index order.
8. Expected client, protocol, and storage failures are explicit error results;
   exceptions are reserved for exceptional construction/allocation failures.

## Store and time model

Each shard owns its map, lazy/active expiration metadata, approximate memory
accounting, and a `std::shared_mutex`. Reads take shared locks; mutations and
lazy deletes take exclusive locks. Multi-key commands lock distinct shards in
pointer order. `KEYS` snapshots each shard briefly instead of holding every lock
while encoding. Values are type-safe variants for strings, integers, lists, and
sets; list/set commands arrive later. Runtime expiration uses monotonic
deadlines plus absolute wall-clock stamps serialized into WAL/snapshot records.

## Mutation ordering

When persistence is enabled, mutating commands follow one ordered path:

```text
validate -> order/encode WAL record -> durability action -> apply mutation
         -> form response
```

Replication publish remains a Phase 5 hook on the same ordering spine.
Acknowledgment guarantees per fsync mode are defined in
[PERSISTENCE.md](PERSISTENCE.md).

## Platform boundary

The public reactor abstraction is platform-neutral:

```text
Linux: epoll + eventfd
macOS: kqueue + EVFILT_USER (or a non-blocking wakeup pipe)
```

Platform headers and behavior stay in implementation files so store, protocol,
and command code remain portable.

## Test boundaries

- Unit tests cover protocol, store, data structures, configuration, WAL, and
  snapshot codecs.
- Integration tests launch real servers on ephemeral ports and exercise
  graceful restart plus crash-style recovery under `fsync=always`.
- ThreadSanitizer validates concurrent components.
- Fuzz targets exercise all untrusted decoders.
- Tests use readiness signals and bounded deadlines instead of timing-only
  sleeps.

# NovaCache Architecture

## Current state

Phase 1 implements the RESP2 value model, incremental parser and encoder,
sharded in-memory store, lazy monotonic TTLs, command registry, RAII POSIX
sockets, blocking server, and one-shot CLI. The server currently processes one
connected client at a time. The reactor, worker pool, locks, and active expiry
shown below are Phase 2 work.

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
and server behavior. Thin programs under `apps/` provide the server, CLI, and
benchmark entry points.
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

Phase 1 shards already own a fixed partition of the keyspace and lazy expiration
metadata. Values are type-safe variants for strings, integers, lists, and sets;
list/set commands arrive later. Runtime expiration decisions use monotonic
deadlines, and each expiry also records an absolute wall-clock timestamp for
future snapshot/WAL serialization so expiration remains meaningful after restart.

## Mutation ordering

Every mutating command will follow one ordered path:

```text
validate -> order/encode WAL record -> durability action -> apply mutation
         -> publish replication record -> form response
```

This common path ensures crash recovery and replicas observe the same mutation
order. Exact acknowledgment guarantees will be documented for each fsync mode.

## Platform boundary

The public reactor abstraction is platform-neutral:

```text
Linux: epoll + eventfd
macOS: kqueue + EVFILT_USER (or a non-blocking wakeup pipe)
```

Platform headers and behavior stay in implementation files so store, protocol,
and command code remain portable.

## Test boundaries

- Unit tests cover protocol, store, data structures, configuration, and disk
  codecs.
- Integration tests launch real servers on ephemeral ports.
- ThreadSanitizer validates concurrent components.
- Fuzz targets exercise all untrusted decoders.
- Tests use readiness signals and bounded deadlines instead of timing-only
  sleeps.

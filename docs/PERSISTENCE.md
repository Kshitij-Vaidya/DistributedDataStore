# NovaCache Persistence

## Status

Phase 3 adds write-ahead logging (WAL), periodic snapshots, and crash recovery.
Persistence is optional and enabled when a data directory is configured.

## Guarantees by fsync mode

| Mode | Acknowledgment guarantee |
| --- | --- |
| `always` | A successful mutating reply means the WAL record was appended and `fsync`ed. |
| `everysec` (default) | A successful reply means the record was appended; it is `fsync`ed within about one second. A crash may lose up to roughly one second of acknowledged writes. |
| `none` | Append only; durability depends on the OS page cache. |

Recovery never invents mutations. Startup loads the newest valid snapshot (if any),
then replays WAL records whose offsets are strictly greater than the snapshot's
stored WAL offset. An incomplete or corrupt WAL **tail** is truncated; earlier
intact records are kept. A corrupt snapshot file is ignored in favor of an older
valid snapshot when present, otherwise recovery starts from an empty store plus
WAL replay.

## On-disk layout

```text
<data_dir>/
  append.ncwal      # write-ahead log
  dump.ncs          # latest durable snapshot
```

### WAL (`append.ncwal`)

Little-endian binary format.

**File header (16 bytes):**

- magic: `NCWAL\0\0\0` (8 bytes)
- version: `u32` (= 1)
- reserved: `u32` (= 0)

**Record:**

- `length` `u32`: bytes of (`offset` + `opcode` + `payload`)
- `offset` `u64`: monotonic record id
- `opcode` `u16`
- `payload`: opcode-specific
- `crc32` `u32`: IEEE CRC-32 over `offset || opcode || payload`

Opcodes:

| Code | Name | Payload |
| ---: | --- | --- |
| 1 | `SET` | `key_len u32`, key bytes, `value_len u32`, value bytes, `has_expiry u8`, optional `expiry_unix_ms i64` |
| 2 | `DEL` | `count u32`, then repeated `key_len u32` + key bytes |
| 3 | `EXPIRE` | `key_len u32`, key bytes, `expiry_unix_ms i64` (absolute; `<= now` deletes) |

Mutations are appended under a single persistence lock so acknowledgment order
matches recovery and future replication order.

### Snapshot (`dump.ncs`)

Little-endian binary format written as `dump.ncs.tmp`, `fsync`ed (file and parent
directory where supported), then atomically renamed to `dump.ncs`.

**Header:**

- magic: `NCSNP\0\0\0` (8 bytes)
- version: `u32` (= 1)
- `wal_offset` `u64`: last WAL offset included in this snapshot
- `entry_count` `u64`

**Entry:**

- `key_len u32`, key bytes
- `type u8` (`0` string, `1` int64, `2` list, `3` set)
- type payload (string: `len u32` + bytes; int64: `i64`; list/set: `count u32` then elements)
- `has_expiry u8`, optional `expiry_unix_ms i64`

**Trailer:** `crc32 u32` over the file contents excluding the trailer.

Snapshots copy each shard briefly under its lock, then release before encoding
the next shard (bounded consistency barrier under the persistence lock so no WAL
append races the snapshot). After a durable snapshot, the WAL is truncated to a
fresh header and continues from `wal_offset + 1`.

## Recovery algorithm

1. Create `data_dir` if needed.
2. Try load `dump.ncs`. On checksum/format failure, treat as missing.
3. Install snapshot entries into the store, skipping keys whose absolute expiry is
   already due at restore wall time. Convert remaining wall expiries into
   monotonic deadlines as `mono_now + (wall_expiry - wall_now)`.
4. Open/create `append.ncwal`. Truncate a corrupt incomplete final record.
5. Replay records with `offset > snapshot.wal_offset` through store primitives
   without appending them again.
6. Resume serving with `next_offset = max(snapshot.wal_offset, last_wal_offset)`.

## Runtime integration

Mutating commands (`SET`, `DEL`, `EXPIRE`) follow:

```text
validate -> encode/order WAL record -> durability action -> apply store mutation
         -> form RESP response
```

Read-only commands do not touch the WAL. Active/lazy expiry removes keys from
memory only; absolute expiry in snapshots/WAL makes those keys absent after
recovery when due.

## Operator notes

Enable persistence with `--data-dir PATH`. Optional flags:

- `--fsync always|everysec|none`
- `--snapshot-interval SECONDS` (`0` disables periodic snapshots; shutdown still
  attempts a final snapshot when persistence is enabled)

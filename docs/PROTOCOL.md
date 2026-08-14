# NovaCache RESP2 Protocol

## Status

This document defines the implemented Phase 2 wire contract. NovaCache accepts
RESP2 clients on its reactor-driven TCP server; unsupported Redis commands and
RESP3 types are outside the current compatibility scope.

## Transport and framing

NovaCache uses TCP, binds to `127.0.0.1:6379` by default, and accepts a
binary-safe subset of RESP2. Frames use CRLF (`\r\n`) terminators. Client
commands are arrays of bulk strings, command names are case-insensitive, and
multiple complete commands may be pipelined on one connection.

Supported values are:

- Simple String: `+OK\r\n`
- Error: `-ERR message\r\n`
- Integer: `:1\r\n`
- Bulk String: `$3\r\nfoo\r\n`
- Null Bulk String: `$-1\r\n`
- Array: `*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n`
- Null Array: `*-1\r\n`

RESP3 types are not supported.

## Initial commands

| Command | Arity | Reply | Notes |
| --- | ---: | --- | --- |
| `PING [message]` | 1-2 | Simple/Bulk String | `PONG` without an argument |
| `ECHO message` | 2 | Bulk String | Returns the argument unchanged |
| `SET key value` | 3 | Simple String | Phase 1 has no NX/XX options |
| `GET key` | 2 | Bulk/Null Bulk | Wrong type is an error |
| `DEL key [key ...]` | 2+ | Integer | Number of removed keys |
| `EXISTS key [key ...]` | 2+ | Integer | Counts each supplied key |
| `EXPIRE key seconds` | 3 | Integer | `1` if set, otherwise `0` |
| `TTL key` | 2 | Integer | Remaining whole seconds |
| `KEYS pattern` | 2 | Array | Currently guarantees `*` only |
| `INFO [section]` | 1-2 | Bulk String | Stable text stats; section is ignored |

### Example

Request:

```text
*3\r\n$3\r\nSET\r\n$4\r\ndemo\r\n$5\r\nvalue\r\n
```

Response:

```text
+OK\r\n
```

A missing `GET` returns `$-1\r\n`.

## Expiration semantics

- `EXPIRE` accepts seconds as a signed 64-bit integer.
- `TTL` returns `-2` when the key does not exist.
- `TTL` returns `-1` when the key has no expiration.
- Runtime deadlines use a monotonic clock.
- Each expiry also stores an absolute wall-clock timestamp for persistence.
- A key observed after its deadline behaves as absent.

## Errors

Errors use RESP2 Error frames. Initial stable categories are:

```text
-ERR unknown command\r\n
-ERR wrong number of arguments\r\n
-ERR value is not an integer or out of range\r\n
-ERR Protocol error\r\n
-WRONGTYPE Operation against a key holding the wrong kind of value\r\n
```

A protocol error closes the connection after the error response when sending a
response remains safe.

## Safety limits

Defaults are defined by `config/novacache.example.conf`:

- Connection input buffer: 16 MiB
- Bulk string: 8 MiB
- Array elements: 1,024
- Nesting depth: 32

The parser must distinguish incomplete data from malformed or oversized data.
Unsupported commands receive an error; NovaCache does not claim complete Redis
compatibility.

Malformed, oversized, or excessively nested frames receive an
`ERR Protocol error` response when safe, after which the server closes that
client connection. Complete pipelined commands are processed in wire order.

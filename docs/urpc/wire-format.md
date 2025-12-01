# Wire Format

## Byte order

All multi-byte integer fields on the wire use **big-endian** (network byte order):

* Conversion functions:

    * `host_to_be<T>(T)` converts host integer/enum to big-endian.
    * `be_to_host<T>(T)` converts big-endian to host.
* Applied to all integral fields in the header and error payload.

## Frame header

On the wire the frame header is **28 bytes** long and has the following layout:

|       Field | Type     | Size (bytes) | Endianness | Description                            |
|------------:|----------|--------------|------------|----------------------------------------|
|     `magic` | `uint32` | 4            | BE         | Magic constant `'URPC'` = `0x55525043` |
|   `version` | `uint8`  | 1            | —          | Protocol version (currently `1`)       |
|      `type` | `uint8`  | 1            | —          | Frame type (`FrameType`)               |
|     `flags` | `uint16` | 2            | BE         | Bitmask of `FrameFlags`                |
| `stream_id` | `uint32` | 4            | BE         | Logical stream identifier              |
| `method_id` | `uint64` | 8            | BE         | Numeric method identifier              |
|    `length` | `uint32` | 4            | BE         | Payload length in bytes                |

The C++ struct `RpcFrameHeader` has an additional `reserved` field and compiler padding, hence
`sizeof(RpcFrameHeader) == 32`, but **`reserved` is not present on the wire and must be ignored** when (de)serializing.

### Magic and version

* `magic` must be exactly `0x55525043` (`'U' 'R' 'P' 'C'`).
* `version` must be `1` for the current implementation.
* Both client and server validate:

    * If `magic != 0x55525043` or `version != 1`, the connection is closed.

### Frame types

Declared in `FrameType`:

```cpp
enum class FrameType : uint8_t
{
    Request  = 0,
    Response = 1,
    Stream   = 2,
    Cancel   = 3,
    Ping     = 4,
    Pong     = 5,
};
```

Semantics:

* **Request** – Client → Server: start an RPC.
* **Response** – Server → Client: final result (success or error).
* **Stream** – Reserved for future streaming use (not used currently).
* **Cancel** – Client → Server: request cancellation of running RPC.
* **Ping** – Either direction: liveness probe.
* **Pong** – Reply to `Ping`, mirrors `stream_id` and `method_id`.

### Flags

Declared as `FrameFlags` bitmask:

```cpp
enum FrameFlags : uint8_t
{
    FLAG_END_STREAM  = 0x01,
    FLAG_ERROR       = 0x02,
    FLAG_COMPRESSED  = 0x04,
};
```

Currently used:

* `FLAG_END_STREAM`:

    * For `Request` and `Response` frames: marks that the sender will not send further frames for this stream.
    * All existing Request/Response/Ping/Pong production frames set `FLAG_END_STREAM`.
* `FLAG_ERROR`:

    * Only meaningful on `Response`.
    * If set, the payload is an **error payload** (see below).
* `FLAG_COMPRESSED`:

    * Reserved for future compressed payload support.
    * Not used yet; must be ignored by receivers.

### Payload

Immediately follows the 28-byte header:

* Size is `length` bytes.
* May be zero (`length == 0`).

Types:

* `Request` payload:

    * Arbitrary binary request body, interpreted by the method implementation.
* `Response` payload:

    * If `FLAG_ERROR` is **not** set: arbitrary binary response body.
    * If `FLAG_ERROR` **is** set: error payload (see below).
* `Cancel`, `Ping`, `Pong`:

    * Current implementation uses an empty payload (`length == 0`).

### Error payload format

When `FrameType::Response` and `FLAG_ERROR` is set, payload has the following binary format:

| Offset | Field         | Type      | Size | Endianness | Description                    |
|-------:|---------------|-----------|------|------------|--------------------------------|
|      0 | `code`        | `uint32`  | 4    | BE         | Application error code         |
|      4 | `message_len` | `uint32`  | 4    | BE         | Length of UTF-8 error message  |
|      8 | `message`     | `char[]`  | N    | —          | Error message bytes            |
|    8+N | `details`     | `uint8[]` | M    | —          | Optional opaque binary details |

Constraints:

* `payload.size() >= 8`
* `payload.size() >= 8 + message_len`

Server builds this format in `RpcConnection::send_simple_error()`, and client decodes it in
`RpcClient::parse_error_payload()`.
# Wire Format

uRPC uses a fixed **28-byte** binary header followed by an optional payload.
All integers are **big-endian**.

The framing format is identical for TCP, TLS, and mTLS transports.

---

# Byte order

All multi-byte integer fields are encoded as **big-endian** ("network byte order").

Utility functions:

```cpp
host_to_be<T>(T value)
be_to_host<T>(T value)
```

Applied to:

* all integers in the header
* all integers in the error payload

---

# Frame header (28 bytes)

The wire header is always **exactly 28 bytes**:

| Field     | Type   | Size | Endianness | Description                         |
|-----------|--------|------|------------|-------------------------------------|
| magic     | uint32 | 4    | BE         | `'URPC'` = `0x55525043`             |
| version   | uint8  | 1    | —          | Protocol version (`1`)              |
| type      | uint8  | 1    | —          | FrameType (Request/Response/Ping/…) |
| flags     | uint16 | 2    | BE         | FrameFlags bitmask                  |
| stream_id | uint32 | 4    | BE         | Logical RPC stream ID               |
| method_id | uint64 | 8    | BE         | Numeric method ID (64-bit hash)     |
| length    | uint32 | 4    | BE         | Payload length in bytes             |

Total: **28 bytes**

### Notes

* The on-wire layout is fully packed (no padding).
* Any frame with invalid magic/version causes the connection to close immediately.

---

# Frame types

```cpp
enum class FrameType : uint8_t
{
    Request  = 0,
    Response = 1,
    Stream   = 2, // reserved
    Cancel   = 3,
    Ping     = 4,
    Pong     = 5,
};
```

Meaning:

* **Request** — client → server: start a new RPC
* **Response** — server → client: final result (success or error)
* **Stream** — reserved, unused
* **Cancel** — client → server: cancel an in-flight RPC
* **Ping** — health probe (either direction)
* **Pong** — reply to Ping, mirrors stream_id/method_id

---

# Flags

```cpp
enum FrameFlags : uint8_t
{
    FLAG_END_STREAM = 0x01,
    FLAG_ERROR      = 0x02,
    FLAG_COMPRESSED = 0x04,
};
```

### Currently used:

* **FLAG_END_STREAM**

    * Set on all Request, Response, Ping, Pong.
    * Indicates no more frames for this stream.

* **FLAG_ERROR**

    * Only meaningful on Response.
    * Payload becomes a binary *error payload* (see below).

* **FLAG_COMPRESSED**

    * Reserved for future use.
    * Must be ignored by receivers.

---

# Payload

Payload immediately follows the **28-byte** header.

```
+-------------------+-----------------------------+
| 28-byte header    | payload[ length ]           |
+-------------------+-----------------------------+
```

* If `length == 0`, no payload is present.
* Payload is **opaque**; the protocol does not impose a serialization format.

### Payload by type

| Type      | Meaning                   |
|-----------|---------------------------|
| Request   | Binary request body       |
| Response  | Success or error payload  |
| Ping/Pong | No payload (`length = 0`) |
| Cancel    | No payload (`length = 0`) |
| Stream    | Reserved                  |

---

# Error payload

If:

```
type == Response
flags has FLAG_ERROR
```

Then payload contains a binary error structure:

| Offset | Field   | Type    | Size | Endianness | Description                    |
|-------:|---------|---------|------|------------|--------------------------------|
|      0 | code    | uint32  | 4    | BE         | Application error code         |
|      4 | msg_len | uint32  | 4    | BE         | UTF-8 message length           |
|      8 | message | char[]  | N    | —          | Human-readable message         |
|    8+N | details | uint8[] | M    | —          | Optional opaque binary details |

Constraints:

* `payload.size() >= 8`
* `payload.size() >= 8 + msg_len`

Construction:

* Server builds this in `RpcConnection::send_simple_error()`
* Client parses this in `RpcClient::parse_error_payload()`

If error is detected:

* `call->error = true`
* `call->response` remains empty

---

# Ping / Pong frames

Used by `RpcClient::async_ping()` and `RpcConnection::handle_ping()`.

### Ping

```
type      = Ping
flags     = END_STREAM
stream_id = unique non-zero id
method_id = 0
length    = 0
payload   = empty
```

### Pong

```
type      = Pong
flags     = END_STREAM
stream_id = same as Ping
method_id = same as Ping
length    = 0
payload   = empty
```

---

# Stream IDs

* 32-bit unsigned integer
* `0` is reserved
* Client assigns stream IDs sequentially
* Response always uses the same `stream_id`
* When `END_STREAM` is set, the logical stream is closed

---

# Method IDs

Method IDs are 64-bit hashes (FNV-1a). Same value is used:

* in Request header
* in Response header

Two ways to produce them:

### Runtime

```cpp
uint64_t id = fnv1a64_rt("Example.Echo");
```

### Compile-time

```cpp
constexpr uint64_t EchoId = method_id("Example.Echo");
```

Compile-time IDs guarantee an identical constant across client/server.

---

# Summary

* uRPC’s wire format is a simple **28-byte header** + binary payload.
* All multibyte integers are **big-endian**.
* Transport layer (TCP/TLS/mTLS) does not change framing.
* Error semantics are fully binary.
* Ping/Pong uses standard frames; no side channels exist.
* Protocol is designed to be trivially parsed and extremely fast.
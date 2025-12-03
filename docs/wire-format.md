# Wire Format

uRPC uses a fixed **28-byte** binary header followed by an optional payload.
All integers are **big-endian**.

The framing format is identical for TCP, TLS, and mTLS transports.

**Important:**  
**Neither TLS nor app-level AES encrypt the uRPC header.  
Only the payload is ever encrypted.**  
The 28-byte header is always sent in plaintext.

If TLS with *app-level AES* is enabled, the payload is additionally encrypted
(AES-256-GCM), but **the wire framing does not change**.

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

* Header has no padding.
* Header is never encrypted (not by TLS, not by AES).
* Any invalid `magic` / `version` closes the connection.

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

* **Request** — client → server
* **Response** — server → client
* **Stream** — reserved
* **Cancel** — cancel running RPC
* **Ping/Pong** — liveness messages

---

# Flags

```cpp
enum FrameFlags : uint8_t
{
    FLAG_END_STREAM = 0x01,
    FLAG_ERROR      = 0x02,
    FLAG_COMPRESSED = 0x04,

    FLAG_TLS        = 0x08, // transport is TLS
    FLAG_MTLS       = 0x10, // mutual TLS (client cert)
    FLAG_ENCRYPTED  = 0x20, // body is AES-GCM encrypted
};
```

### Currently used:

**FLAG_END_STREAM**
*Set on all Request/Response/Ping/Pong.*
Marks the end of a logical stream.

**FLAG_ERROR**
Payload is an error payload.

**FLAG_COMPRESSED**
Reserved.

**FLAG_TLS**
Underlying connection uses TLS.
Header still remains plaintext.

**FLAG_MTLS**
TLS connection with verified client certificate.
Header still remains plaintext.

**FLAG_ENCRYPTED**
Payload is encrypted with **AES-256-GCM**.
Header is not encrypted.

---

# Payload

Payload immediately follows the fixed header.

If `FLAG_ENCRYPTED`:

```
payload = IV[12] + ciphertext[...] + TAG[16]
```

Otherwise:

```
payload = raw binary
```

### Layout

```
+-------------------+-----------------------------+
| 28-byte header    | payload[ length ]           |
+-------------------+-----------------------------+
```

* `length == 0`: no payload
* payload format = depends on type + flags

**TLS note:**
TLS encrypts the TCP stream but **does not encrypt or alter the uRPC header**.

---

# Payload by type

| Type      | Meaning                       |
|-----------|-------------------------------|
| Request   | Raw or AES-encrypted data     |
| Response  | Raw/AES body or error payload |
| Ping/Pong | Always empty                  |
| Cancel    | Empty                         |
| Stream    | Reserved                      |

---

# Encrypted payload (FLAG_ENCRYPTED)

If:

```
flags has FLAG_ENCRYPTED
```

the payload has 3 components:

| Component | Size     | Description            |
|----------:|----------|------------------------|
|        IV | 12 bytes | GCM nonce              |
|        CT | N bytes  | Ciphertext             |
|       TAG | 16 bytes | GCM authentication tag |

`length = 12 + N + 16`.

### AES key origin

Derived from TLS exporter:

```cpp
SSL_export_keying_material(
    ssl, key, 32,
    "urpc_app_key_v1",
    ...);
```

Constraints:

* Works only when TLS/mTLS is active.
* Requires `app_encryption = true` on both endpoints.
* Header is not encrypted.

---

# Error payload

If:

```
type == Response
flags has FLAG_ERROR
```

then (after AES decrypt if `FLAG_ENCRYPTED`):

| Offset | Field   | Type    | Size | Endianness | Description            |
|-------:|---------|---------|------|------------|------------------------|
|      0 | code    | uint32  | 4    | BE         | Application error code |
|      4 | msg_len | uint32  | 4    | BE         | UTF-8 message length   |
|      8 | message | char[]  | N    | —          | UTF-8 text             |
|    8+N | details | uint8[] | M    | —          | Optional data          |

---

# Ping / Pong frames

Always unencrypted and with no payload.

### Ping

```
type      = Ping
flags     = FLAG_END_STREAM (+ FLAG_TLS / FLAG_MTLS)
stream_id = unique (>0)
method_id = 0
length    = 0
```

### Pong

```
type      = Pong
flags     = FLAG_END_STREAM (+ TLS bits)
stream_id = same as Ping
method_id = same as Ping
length    = 0
```

---

# Stream IDs

* 32-bit unsigned
* `0` reserved
* client increments sequentially
* Response echoes the same ID
* END_STREAM closes logical stream

---

# Method IDs

64-bit FNV-1a.

Runtime:

```cpp
uint64_t id = fnv1a64_rt("Example.Echo");
```

Compile-time:

```cpp
constexpr uint64_t EchoId = method_id("Example.Echo");
```

---

# Summary

* uRPC wire format = **28-byte plaintext header + payload**.
* Header is **never encrypted** (not by TLS, not by AES).
* Payload may be:

    * raw
    * TLS-protected (transport layer)
    * AES-256-GCM encrypted (`FLAG_ENCRYPTED`)
* Transport does not affect framing.
* Ping/Pong are minimal, empty frames.
* Protocol remains fast, deterministic, simple to parse.
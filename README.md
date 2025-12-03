# **uRPC Wire Protocol**

uRPC is a minimal binary RPC protocol designed for high-performance async runtimes (**uvent**).
It uses a compact big-endian header and fully supports multiplexing: many concurrent RPCs over a single connection.

Every message on the wire is a **Frame**:

```

+-------------------+-----------------------------+
| Header (fixed)    | Payload (binary, length N) |
+-------------------+-----------------------------+

```

---

## Header

**Size:** 28 bytes  
**Byte order:** Big-Endiann

Layout:

```text
uint32  magic       // 'URPC' = 0x55525043
uint8   version     // protocol version, currently 1
uint8   type        // frame type (see FrameType table)
uint16  flags       // bitmask (END_STREAM, ERROR, COMPRESSED, …)
uint32  reserved    // must be 0; reserved for future protocol extensions
uint32  stream_id   // logical RPC stream ID
uint64  method_id   // FNV1a64("Service.Method")
uint32  length      // payload size in bytes
```

The `reserved` field is included in the header but is not used by the current
protocol version. Implementations MUST send it as zero and MUST ignore its value
when receiving.

**Important:**
TLS/mTLS encrypt the *transport stream*, but the protocol header is still parsed
normally because OpenSSL decrypts before delivering bytes to `async_read`.
App-level AES (if enabled) encrypts **only the payload**, never the header.

---

## Frame types

```text
0 = Request
1 = Response
2 = Stream   // reserved for future streaming
3 = Cancel
4 = Ping
5 = Pong
```

| Type | Name     | Direction | Description                |
|------|----------|-----------|----------------------------|
| 0    | Request  | C → S     | RPC request                |
| 1    | Response | S → C     | RPC response               |
| 2    | Stream   | S → C     | Streaming chunk (reserved) |
| 3    | Cancel   | C → S     | Cooperative cancellation   |
| 4    | Ping     | C ↔ S     | Health check               |
| 5    | Pong     | C ↔ S     | Health check response      |

---

## Flags

`flags` is a bitmask:

* `0x01` — `END_STREAM`
* `0x02` — `ERROR`
* `0x04` — `COMPRESSED`
* `0x08` — `TLS` (transport is TLS)
* `0x10` — `MTLS` (mutual TLS)
* `0x20` — `ENCRYPTED` (payload encrypted with app-level AES)

Rules:

* Request frames MUST NOT set `ERROR`.
* Response frames MUST set `ERROR` when they carry an error payload.
* AES and TLS flags **never** imply header encryption — only payload (AES) or entire transport (TLS).

---

## Request / Response Flow

1. Client opens a connection (TCP or TLS/mTLS).
2. Client sends a Request with unique `stream_id` and `method_id`.
3. Server dispatches to registered handler.
4. Handler returns a binary vector.
5. Server sends Response with same `stream_id`.
6. Client matches by `stream_id`.

Multiplexing allows many in-flight RPCs on one connection.

---

## Errors

Errors are encoded as binary payloads.

```
+-----------------------+-----------------------+---------------------------+
| uint32 code (BE)      | uint32 msg_len (BE)   | msg[msg_len] | details[] |
+-----------------------+-----------------------+---------------------------+
```

### Client reaction

If ERROR flag is set:

* client marks call as failed,
* extracts code/message,
* the returned vector is empty.

---

## Ping / Pong

Used for liveness checks.

**Ping:**

```
type      = 4
flags     = END_STREAM
stream_id = non-zero
length    = 0
```

**Pong:**

```
type      = 5
flags     = END_STREAM
stream_id = same as Ping
length    = 0
```

---

## Stream IDs

* `0` is reserved
* client allocates increasing non-zero IDs
* response must reuse request's ID
* stream closes on END_STREAM

---

## Method IDs

64-bit FNV1a:

```cpp
uint64_t id = urpc::method_id("Example.Echo");
```

Compile-time:

```cpp
server.register_method_ct<urpc::method_id("Example.Echo")>(handler);
```

---

# **Optional TLS and mTLS**

Enabling TLS/mTLS is purely a transport decision:

* framing stays the same
* header remains plaintext at the protocol level
* OpenSSL decrypts before `async_read`
* handler logic unchanged

mTLS additionally verifies client certificates.

---

# **App-level AES Encryption**

If enabled (both sides agree):

* a per-connection AES-256-GCM key is derived via TLS exporter
* payload is replaced by:

```
IV[12] + ciphertext[N] + TAG[16]
```

* header is **never encrypted**
* flags include `ENCRYPTED`

AES works **on top of TLS** or plain TCP (if you provide your own key).

---

# **Command-line client (urpc_cli)**

```bash
urpc_cli --host 127.0.0.1 --port 45900 --method Example.Echo --data "hello"
```

TLS:

```bash
urpc_cli --tls --tls-ca ca.crt \
         --tls-server-name localhost \
         --host 127.0.0.1 --port 45900 \
         --method Example.Echo --data "hi"
```

mTLS:

```bash
urpc_cli --tls \
         --tls-ca ./ca.crt \
         --tls-cert ./client.crt \
         --tls-key ./client.key \
         --tls-server-name localhost \
         --method Example.Echo \
         --data "hello secure"
```

AES:

```bash
urpc_cli --aes --aes-key hex:001122334455...
```

---

## Payload

Payload is opaque and length-prefixed:

* JSON
* Protobuf
* MsgPack
* custom binary formats

---

# Licence

uRPC is distributed under the [MIT license](LICENSE)
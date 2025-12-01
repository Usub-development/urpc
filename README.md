# uRPC Wire Protocol

uRPC is a minimal binary RPC protocol over TCP, designed for high-performance async runtimes (uvent).  
The protocol is compact, big-endian, and fully multiplexed.

Every message on the wire is a **Frame**:

```

+-------------------+-----------------------------+
| Header (fixed)    | Payload (binary, length N) |
+-------------------+-----------------------------+

```

---

## Header

**Size:** 28 bytes  
**Byte order:** Big-Endian

Layout:

```text
uint32  magic       // 'URPC' = 0x55525043
uint8   version     // protocol version, currently 1
uint8   type        // frame type (see FrameType table)
uint16  flags       // bitmask (END_STREAM, ERROR, COMPRESSED, …)
uint32  reserved    // must be 0; reserved for future protocol extensions
uint32  stream_id   // logical stream id (similar to HTTP/2 stream id)
uint64  method_id   // FNV1a64("Service.Method") for requests; echoed in responses
uint32  length      // payload size in bytes
```

The `reserved` field is included in the header (total size = 32 bytes),
but is not used by the current protocol version. Implementations MUST
send it as zero and MUST ignore its value when receiving.

---

## Frame types

```text
0 = Request
1 = Response
2 = Stream   // reserved for future streaming responses
3 = Cancel   // client asks server to cancel a running call
4 = Ping     // liveness probe
5 = Pong     // response to Ping
```

| Type | Name     | Direction | Description                       |
|------|----------|-----------|-----------------------------------|
| 0    | Request  | C → S     | RPC request                       |
| 1    | Response | S → C     | RPC response                      |
| 2    | Stream   | S → C     | Chunk of streaming response (TBD) |
| 3    | Cancel   | C → S     | Cooperative cancellation          |
| 4    | Ping     | C ↔ S     | Health check                      |
| 5    | Pong     | C ↔ S     | Health check response             |

---

## Flags

`flags` is a bitmask:

* `0x01` — `END_STREAM`
* `0x02` — `ERROR` (Response indicates an application error)
* `0x04` — `COMPRESSED` (payload is compressed)

Rules:

* Request frames MUST NOT set `ERROR`.
* Response frames set `ERROR` if they contain an error payload.

---

## Request / Response Flow

1. Client opens a TCP connection.

2. Client sends a `Request` frame:

    * `type = 0`
    * `stream_id != 0`
    * `method_id = FNV1a64("Service.Method")`
    * `payload = request body` (arbitrary binary)

3. Server invokes the registered handler:

```cpp
using RpcHandlerFn =
    usub::uvent::task::Awaitable<std::vector<uint8_t>>(
        RpcContext&, std::span<const uint8_t>);
```

4. Handler returns a binary response via `co_return`.
5. Server sends a `Response` frame with the returned payload.
6. Client receives the `Response` and resumes the awaiting coroutine.

---

## Errors

Errors are encoded as **binary error payloads**, not JSON.

If `ERROR` flag is set, the payload structure is:

```
+-----------------------+-----------------------+---------------------------+
| uint32 code (BE)      | uint32 msg_len (BE)   | msg[msg_len] | details[] |
+-----------------------+-----------------------+---------------------------+
```

All integers are **big-endian**.

### Error payload format

| Field   | Type   | Description                       |
|---------|--------|-----------------------------------|
| code    | u32 BE | Application-defined error code    |
| msg_len | u32 BE | Length of the UTF-8 error message |
| message | bytes  | Human-readable error message      |
| details | bytes  | Optional opaque binary blob       |

### Example: server error

```cpp
co_await send_simple_error(ctx, 404, "Unknown method");
```

Produces a `Response` frame:

* `type = Response`
* `flags = END_STREAM | ERROR`
* `payload = binary error payload`
* `stream_id = original stream`
* `method_id = original method`

### Client reaction

On `ERROR`:

* The client marks the pending call as failed.
* The response vector is returned **empty**.
* If desired, the user can decode the error payload manually.

---

## Ping / Pong

Used for connection health checks.

**Ping (client → server):**

```
type      = 4
flags     = END_STREAM
stream_id = arbitrary non-zero
length    = 0
payload   = empty
```

**Pong (server → client):**

```
type      = 5
flags     = END_STREAM
stream_id = same as Ping
length    = 0
payload   = empty
```

`RpcConnection::handle_ping` automatically sends `Pong`.

---

## Stream IDs

* `stream_id = 0` is reserved.
* Every in-flight RPC uses a unique non-zero `stream_id`.
* A `Response` must use the same `stream_id` as the `Request`.
* When `END_STREAM` is set, the logical stream is closed.

---

## Method IDs

Method IDs are 64-bit FNV1a hashes:

```cpp
uint64_t id = urpc::method_id("Example.Echo");
```

Server registration:

```cpp
server.register_method_ct<urpc::method_id("Example.Echo")>(
    [](urpc::RpcContext& ctx,
       std::span<const uint8_t> body)
    -> usub::uvent::task::Awaitable<std::vector<uint8_t>>
    {
        std::vector<uint8_t> out(body.begin(), body.end());
        co_return out;
    });
```

The response always reuses the request’s `method_id`.

---

## Payload

uRPC does not enforce a serialization format.
The payload is an opaque, length-prefixed binary blob:

* JSON
* MsgPack
* Protobuf
* raw structs
* custom binary formats

`RpcClient` and `RpcConnection` only handle framing.
Application code only deals with:

```cpp
std::span<const uint8_t>
std::vector<uint8_t>
```

No assumptions about content.
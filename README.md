# uRPC Wire Protocol

uRPC is a minimal binary RPC protocol over TCP, designed for high-performance async runtimes (uvent).

Every message on the wire is a **Frame**:

```
+-------------------+-----------------------------+
| Header (fixed)    | Payload (binary, length N) |
+-------------------+-----------------------------+
```

---

## Header

**Size:** 24 bytes  
**Byte order:** Big-Endian

Layout:

```text
uint32  magic       // 'URPC' = 0x55525043
uint8   version     // protocol version, currently 1
uint8   type        // frame type (see FrameType table)
uint8   flags       // bitmask (END_STREAM, COMPRESSED, etc.)
uint8   reserved    // must be 0
uint32  stream_id   // logical stream id (like HTTP/2 stream id)
uint64  method_id   // FNV1a64("Service.Method") for requests, echoed in responses
uint32  length      // payload size in bytes
```

### Frame types

```text
0 = Request
1 = Response
2 = Stream   // reserved for future streaming responses
3 = Cancel   // client asks server to cancel a running call
4 = Ping     // liveness probe
5 = Pong     // response to Ping
```

| Type | Name     | Direction | Description                         |
|------|----------|-----------|-------------------------------------|
| 0    | Request  | C → S     | RPC request                         |
| 1    | Response | S → C     | RPC response                        |
| 2    | Stream   | S → C     | Chunk of a streaming response (TBD) |
| 3    | Cancel   | C → S     | Cooperative cancellation for a call |
| 4    | Ping     | C ↔ S     | Health / liveness check             |
| 5    | Pong     | C ↔ S     | Reply to Ping                       |

### Flags

`flags` is a bitmask:

- `0x01` — `END_STREAM`
- `0x02` — `ERROR` (Response is an error frame)
- `0x04` — `COMPRESSED` (payload is compressed)

For Request frames, `ERROR` MUST be 0.
For Response frames, `ERROR != 0` indicates an error payload.

---

## Request / Response flow

1. Client opens a TCP connection.

2. Client sends a `Request` frame:

    * `type = 0` (Request)
    * `stream_id != 0`
    * `method_id = FNV1a64("Service.Method")`
    * `payload = request body` (arbitrary binary: JSON, MsgPack, Proto, etc.)

3. On the server, the registered handler is invoked:

   ```cpp
   using RpcHandlerFn = usub::uvent::task::Awaitable<std::vector<uint8_t>>(
       RpcContext&, std::span<const uint8_t> request_body);
   ```

   The handler receives:

    * `RpcContext` (stream id, method id, cancellation token, etc.)
    * raw request body

   and returns a `std::vector<uint8_t>` as the response payload via `co_return`.

4. `RpcConnection` builds a `Response` frame:

    * `type = 1` (Response)
    * `stream_id = same as request`
    * `method_id = same as request`
    * `payload = handler return value`

5. Client receives the `Response` frame and reads the payload.

---

## Errors

Errors are encoded as normal `Response` frames with a JSON payload:

```json
{
  "error_message": "Unknown method"
}
```

Server side helper (`send_simple_error`) builds that JSON, then sends a `Response` with:

* `type = 1`
* `stream_id = same as request`
* `method_id = same as request`
* `flags = END_STREAM`
* `payload = error JSON`

---

## Ping / Pong

Ping/Pong is used to check connection liveness.

**Ping (client → server):**

```text
type      = 4 (Ping)
stream_id = arbitrary non-zero id
length    = 0
payload   = empty
```

**Pong (server → client):**

```text
type      = 5 (Pong)
stream_id = same as Ping
length    = 0
payload   = empty
```

The server implementation handles `Ping` and sends `Pong` from inside `RpcConnection::handle_ping`.

---

## Stream IDs

* `stream_id = 0` is reserved and MUST NOT be used for application calls.
* Each in-flight RPC call uses a non-zero `stream_id`.
* The response **must** use the same `stream_id` as the request.
* After a `Response` with `END_STREAM` the logical stream is closed; a new call must use a new `stream_id`.

---

## Method IDs

Method IDs are 64-bit FNV-1a hashes computed from the textual method name:

```cpp
uint64_t id = urpc::method_id("Example.Echo");
```

On the server:

```cpp
server.register_method_ct<urpc::method_id("Example.Echo")>(
    [](urpc::RpcContext& ctx,
       std::span<const uint8_t> body)
    -> usub::uvent::task::Awaitable<std::vector<uint8_t>>
    {
        // Echo implementation
        std::vector<uint8_t> out(body.begin(), body.end());
        co_return out;
    });
```

On the wire, the server **echoes the same `method_id`** in the `Response` header, so the client can correlate it with
the original call if needed.

---

## Payload

The protocol does not prescribe a specific serialization format.
`payload` is just a `length`-prefixed binary blob:

* JSON
* MsgPack
* Protobuf
* FlatBuffers
* custom binary structs

All framing is handled by `RpcClient` / `RpcConnection`, user code only deals with `std::span<const uint8_t>` and
`std::vector<uint8_t>`.
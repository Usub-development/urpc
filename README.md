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

1. Client opens a connection (TCP or TLS/mTLS).
2. Client sends a **Request** with a unique stream ID and method ID.
3. Server invokes the handler associated with the method ID.
4. Handler returns `std::vector<uint8_t>` as binary response.
5. Server sends a **Response** with the same stream ID and method ID.
6. Client matches the response and resumes the awaiting coroutine.

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

# **Optional TLS and mTLS**

uRPC supports switching transports without recompiling the protocol:

* **Plain TCP**
* **TLS**
* **Mutual TLS (mTLS)** — both client and server verify certificates

Transport selection is configured in:

* `RpcClientConfig`
* `RpcServerConfig`

When TLS/mTLS is enabled:

* Transport automatically wraps the underlying TCP socket
* No changes to protocol, wire format, or handlers
* No difference in Request/Response API

Everything above remains identical.

## Example mTLS certificate generation

1. Create certs folder:
```bash
mkdir certs
cd certs 
```
2. CA:
```bash
# CA key
openssl genrsa -out ca.key 4096

# CA cert (self-signed)
openssl req -x509 -new -nodes \
  -key ca.key \
  -sha256 -days 3650 \
  -out ca.crt \
  -subj "/C=XX/ST=TestState/L=TestCity/O=urpc-CA/OU=Dev/CN=urpc-test-ca"
```
3. Server certificate:
```bash
# Server key
openssl genrsa -out server.key 2048

# CSR
openssl req -new \
  -key server.key \
  -out server.csr \
  -subj "/C=XX/ST=TestState/L=TestCity/O=urpc-Server/OU=Dev/CN=localhost"

openssl x509 -req \
  -in server.csr \
  -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out server.crt \
  -days 365 -sha256
```
4. Client certificate:
```bash
# Client key
openssl genrsa -out client.key 2048

# CSR
openssl req -new \
  -key client.key \
  -out client.csr \
  -subj "/C=XX/ST=TestState/L=TestCity/O=urpc-Client/OU=Dev/CN=urpc-client"

openssl x509 -req \
  -in client.csr \
  -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out client.crt \
  -days 365 -sha256
```

---

# **Command-line client (urpc_cli)**

uRPC includes a small coroutine-based CLI for testing RPC endpoints.

### Example (TCP)

```bash
urpc_cli \
  --host 127.0.0.1 \
  --port 45900 \
  --method Example.Echo \
  --data "hello"
```

### Example (TLS)

```bash
urpc_cli \
  --host localhost \
  --port 45900 \
  --method Example.Echo \
  --data "hello over TLS" \
  --tls \
  --tls-ca ./ca.crt \
  --tls-server-name localhost
```

### Example (mTLS)

```bash
urpc_cli \
  --host localhost \
  --port 45900 \
  --method Example.Echo \
  --data "hello from mTLS" \
  --tls \
  --tls-ca ./ca.crt \
  --tls-cert ./client.crt \
  --tls-key ./client.key \
  --tls-server-name localhost
```

The CLI prints both UTF-8 and hex output for debugging:

```
---- RESPONSE (utf8) ----
hello from mTLS

---- RESPONSE (hex) ----
68 65 6c 6c 6f …
```

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

---

# Licence

uRPC is distributed under the [MIT license](LICENSE)
# Server Behaviour

This section describes how the server side of uRPC operates:
`RpcServer`, `RpcConnection`, and `RpcMethodRegistry`.

It covers:

- TCP / TLS / mTLS server transports
- Accept loop
- Per-connection RPC handling
- Method registry
- Cancellation
- Response/error handling
- Behaviour when app-level AES is enabled

---

# **Important encryption note**

The uRPC **28-byte header is never encrypted**.

* TLS/mTLS encrypts the transport stream, but the header remains in plaintext at the uRPC framing layer.
* App-level AES (FLAG_ENCRYPTED) **only encrypts the payload**, never the header.

The server always receives the header unencrypted and can read all fields before decrypting payload (if needed).

---

# **RpcServer**

`RpcServer` owns:

* Listener address (host/port)
* Worker thread count
* Method registry
* Transport factory (`IRpcStreamFactory`) — TCP, TLS, mTLS

```cpp
struct RpcServerConfig {
    std::string host;
    uint16_t    port;
    int         threads = 1;
    std::shared_ptr<IRpcStreamFactory> stream_factory;
};
```

If `stream_factory` is null, the server uses plain TCP (`TcpRpcStreamFactory`).

---

## **Running the server**

Two entry points:

### `Awaitable<void> RpcServer::run_async()`

Asynchronous accept loop.

### `void RpcServer::run()`

* Creates `usub::Uvent` with N worker threads
* Spawns `run_async()`
* Calls `uvent.run()`

This is the standard standalone entry point.

---

# **Accept loop**

`RpcServer::accept_loop()`:

1. Create listening socket:

   ```cpp
   net::TCPServerSocket acceptor(host_.c_str(), port_);
   ```

2. Infinite loop:

   ```cpp
   while (true) {
       auto soc = co_await acceptor.async_accept();
   }
   ```

3. On failure:

    * log
    * sleep 50ms
    * continue

4. On success:

    * Wrap into transport stream:

      ```cpp
      auto stream = stream_factory->create_server_stream(std::move(soc.value()));
      ```

      For TLS/mTLS this performs a **server-side TLS handshake**.

    * Create connection:

      ```cpp
      auto conn = std::make_shared<RpcConnection>(stream, registry_);
      ```

    * Spawn read loop:

      ```cpp
      system::co_spawn(RpcConnection::run_detached(conn));
      ```

Each accepted connection gets:

* its own `RpcConnection`
* its own coroutine for frame reading
* its own write mutex

---

# **RpcMethodRegistry**

Maps:

```
method_id → RpcHandlerFn*
```

Handler signature:

```cpp
Awaitable<std::vector<uint8_t>>(
    RpcContext&, std::span<const uint8_t>);
```

API:

```cpp
void register_method(uint64_t id, RpcHandlerPtr fn);
void register_method(std::string_view name, RpcHandlerPtr fn); // runtime hash
RpcHandlerPtr find(uint64_t id) const;
```

Compile-time:

```cpp
template<uint64_t MethodId, typename F>
void register_method_ct(F&& f);
```

---

# **RpcContext**

Passed to every handler:

```cpp
struct RpcContext {
    IRpcStream& stream;
    uint32_t    stream_id;
    uint64_t    method_id;
    uint16_t    flags;
    CancellationToken cancel_token;
};
```

Contains:

* stream metadata
* transport flags (TLS, mTLS, ENCRYPTED)
* cancellation token

---

# **RpcConnection**

One per TCP/TLS/mTLS connection.

Responsibilities:

* Read frames
* Parse header (always plaintext)
* Decrypt payload if `FLAG_ENCRYPTED`
* Dispatch handler
* Handle cancellation
* Send responses or errors
* Handle Ping/Pong

Fields:

```cpp
std::shared_ptr<IRpcStream> stream_;
RpcMethodRegistry& registry_;
AsyncMutex write_mutex_;
AsyncMutex cancel_map_mutex_;
std::unordered_map<uint32_t,
    std::shared_ptr<CancellationSource>> cancel_map_;
```

---

## **Lifecycle**

Entrypoint:

```cpp
static Awaitable<void> run_detached(std::shared_ptr<RpcConnection> self);
```

Executes:

```cpp
co_await self->loop();
```

On return:

* connection finishes
* stream is closed

---

# **Main loop**

`RpcConnection::loop()`:

1. Validate stream
2. Loop:

    * read **28-byte plaintext header**
    * parse & validate
    * read payload
    * if `FLAG_ENCRYPTED` → decrypt AES-GCM
    * dispatch by frame type

---

# **Request Handling**

```cpp
Awaitable<void> handle_request(RpcFrame frame);
```

Steps:

1. Lookup handler
2. If missing → send error 404
3. Create `CancellationSource`
4. Build `RpcContext`
5. Call handler
6. Remove cancel source
7. Send success response

---

# **Cancel Handling**

A frame with:

```
type = Cancel
```

Causes:

* lookup cancellation source
* if found → request cancellation

Handlers must check `ctx.cancel_token`.

---

# **Ping Handling**

`handle_ping(frame)` always replies with Pong:

```
type      = Pong
flags     = END_STREAM (+ TLS/MTLS bits)
stream_id = same
method_id = same
```

---

# **Ignored types**

Server ignores:

* `Response`
* `Stream` (reserved)
* `Pong`
* unknown

Only logs them.

---

# **Sending frames**

### `locked_send(hdr, body)`

Serializes writes:

```cpp
AsyncMutex lock(write_mutex_);
send_frame(*stream_, hdr, body);
```

### `send_response(ctx, body)`

Builds:

```
type      = Response
flags     = END_STREAM
stream_id = ctx.stream_id
method_id = ctx.method_id
length    = body.size()
```

Then dispatches with `locked_send`.

### `send_simple_error`

Builds binary error structure:

```
u32 code
u32 msg_len
msg bytes
```

Sent with:

```
flags = END_STREAM | ERROR
```

---

# **TLS / mTLS on the server**

Server uses TLS if a TLS factory is supplied:

```cpp
config.stream_factory =
    std::make_shared<TlsRpcStreamFactory>(TlsServerSettings{
        .ca   = "ca.crt",
        .cert = "server.crt",
        .key  = "server.key",
        .require_client_cert = true, // mTLS
    });
```

Handshake occurs inside:

```
create_server_stream(...)
```

If handshake fails → connection discarded.

### Header visibility under TLS

Even with TLS/mTLS:

* TLS encrypts the TCP stream
* uRPC **still receives the header as plaintext** at framing level
  (the bytes are decrypted by TLS *before* uRPC reads them)

From uRPC perspective the header is always readable and unencrypted.

---

# **App-level AES on the server**

If `FLAG_ENCRYPTED`:

* server decrypts **only the payload**
* header stays readable

AES-256-GCM key is derived from TLS exporter:

```cpp
SSL_export_keying_material(... "urpc_app_key_v1" ...)
```

Used only if both server and client enable app-level encryption.

---

# **Summary**

* Server handles TCP, TLS, and mTLS transports.
* TLS/mTLS do **not** encrypt uRPC headers; only raw TCP bytes are TLS-protected.
* App-level AES encrypts **only the payload**, never the header.
* Each client connection has its own `RpcConnection`.
* Requests are multiplexed by `stream_id`.
* Cancellation is cooperative.
* Ping/Pong built-in.
* Method registry is hash-based.
* Transport layer is pluggable.
* Flow is fully coroutine-driven under uvent.
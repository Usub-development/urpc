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

Runs the asynchronous accept loop.

### `void RpcServer::run()`

* Creates a `usub::Uvent` instance with `threads_` worker threads.
* Spawns `run_async()` via `system::co_spawn`.
* Calls `uvent.run()` to start the event loop.

This is the typical entry point for a standalone server.

---

# **Accept loop**

`RpcServer::accept_loop()` performs:

1. Creates the listening socket:

   ```cpp
   net::TCPServerSocket acceptor(host_.c_str(), port_);
   ```

2. Infinite loop:

   ```cpp
   while (true) {
       auto soc = co_await acceptor.async_accept();
   }
   ```

3. On failed accept:

    * Log
    * Sleep 50ms (`this_coroutine::sleep_for`)
    * Continue

4. On success:

    * Convert accepted TCP socket → `IRpcStream` using `stream_factory`

      ```cpp
      auto stream = stream_factory->create_server_stream(std::move(soc.value()));
      ```

      With TLS/mTLS factory this performs a **server-side TLS handshake**.

    * Create connection:

      ```cpp
      auto conn = std::make_shared<RpcConnection>(stream, registry_);
      ```

    * Spawn read loop:

      ```cpp
      system::co_spawn(RpcConnection::run_detached(conn));
      ```

Each accepted connection gets its own:

* `RpcConnection` instance
* Independent coroutine for reading frames
* Independent write serialization mutex

---

# **RpcMethodRegistry**

Maps:

```
method_id → function pointer (RpcHandlerFn*)
```

Handler type:

```cpp
using RpcHandlerFn =
    usub::uvent::task::Awaitable<std::vector<uint8_t>>(
        RpcContext&, std::span<const uint8_t>);
```

API:

```cpp
void register_method(uint64_t method_id, RpcHandlerPtr fn);
void register_method(std::string_view name, RpcHandlerPtr fn); // runtime hash
RpcHandlerPtr find(uint64_t method_id) const;
```

Compile-time registration:

```cpp
template<uint64_t MethodId, typename F>
void register_method_ct(F&& f);
```

Server forwards registry access:

```cpp
RpcMethodRegistry& registry();
```

---

# **RpcContext**

Passed to all handlers:

```cpp
struct RpcContext {
    IRpcStream& stream;
    uint32_t    stream_id;
    uint64_t    method_id;
    uint16_t    flags;
    CancellationToken cancel_token;
};
```

Purpose:

* Identify which stream the response must be sent on
* Access transport for optional out-of-band writes
* Propagate cooperative cancellation

---

# **RpcConnection**

A `RpcConnection` manages a single TCP/TLS/mTLS connection.

Responsibilities:

* Read frames
* Dispatch requests
* Manage per-RPC cancellation
* Send responses and errors
* Handle Ping/Pong

Key fields:

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

Connection entrypoint:

```cpp
static Awaitable<void> run_detached(std::shared_ptr<RpcConnection> self);
```

This awaits:

```cpp
co_await self->loop();
```

On return:

* The connection is done
* Transport will be shut down by caller or by read loop

---

# **Main loop**

`RpcConnection::loop()`:

1. Validate `stream_`
2. Loop forever:

    * Read header (**28 bytes**)
    * Parse header
    * Validate magic/version
    * Read payload (if any)
    * Dispatch by frame type

---

# **Request Handling**

```cpp
Awaitable<void> handle_request(RpcFrame frame);
```

Steps:

1. Lookup handler:

   ```cpp
   auto fn = registry_.find(method_id);
   ```

2. If not found:

    * Send error response with:

      ```
      code = 404
      message = "Unknown method"
      ```
    * Return

3. Create a `CancellationSource` for this RPC:

   ```cpp
   cancel_map_[stream_id] = src;
   ```

4. Build `RpcContext`:

   ```cpp
   ctx.stream       = *stream_;
   ctx.stream_id    = header.stream_id;
   ctx.method_id    = header.method_id;
   ctx.flags        = header.flags;
   ctx.cancel_token = src->token();
   ```

5. Invoke handler:

   ```cpp
   auto resp = co_await (*fn)(ctx, payload_span);
   ```

6. Remove cancellation source:

   ```cpp
   cancel_map_.erase(stream_id);
   ```

7. Send success:

   ```cpp
   send_response(ctx, resp);
   ```

---

# **Cancel Handling**

Triggered by frame:

```
type = FrameType::Cancel
```

`handle_cancel(frame)`:

1. Lookup `CancellationSource` in `cancel_map_`
2. If found:

    * Remove it
    * Call `src->request_cancel()`
3. Otherwise:

    * Log missing source

Handlers must cooperatively observe `ctx.cancel_token`.

---

# **Ping Handling**

`handle_ping(frame)`:

* Always sends a matching Pong:

```
type = Pong
flags = END_STREAM
stream_id = same
method_id = same
payload = empty
```

Uses `locked_send` to serialize write.

---

# **Pong / Unknown Types**

Server ignores:

* `Response`
* `Stream`
* `Pong`
* Unknown types

Only logs them.

---

# **Sending frames**

### `locked_send(hdr, body)`

* Acquires `write_mutex_`
* Calls `send_frame(*stream_, hdr, body)`

Guarantees **atomicity** of each frame on the wire.

### `send_response(ctx, body)`

Builds:

```
type      = Response
flags     = END_STREAM
stream_id = ctx.stream_id
method_id = ctx.method_id
length    = body.size()
```

Then calls `locked_send`.

### `send_simple_error(ctx, code, msg)`

Builds an error payload:

```
u32: code
u32: msg_len
msg bytes
(details unused)
```

And sends with:

```
flags = END_STREAM | ERROR
```

---

# **TLS / mTLS on the server**

Server TLS is enabled by providing a TLS stream factory:

```cpp
config.stream_factory =
    std::make_shared<TlsRpcStreamFactory>(TlsServerSettings{
        .ca         = "ca.crt",        // optional (required for mTLS)
        .cert       = "server.crt",
        .key        = "server.key",
        .require_client_cert = true,    // mTLS
    });
```

The accept loop performs:

* TCP accept
* TLS handshake inside `create_server_stream`
* If handshake fails → connection closed
* On success → `RpcConnection` created

mTLS validation includes:

* Proper client certificate
* CA chain validation
* SAN/Hostname checks (optional for client certs)
* Access to full peer certificate via `stream_->peer_certificate()`

Everything below the abstract `IRpcStream` is transport-specific.

---

# **Summary**

* Server accepts TCP or TLS/mTLS connections.
* Each connection gets its own `RpcConnection`.
* All requests are fully multiplexed by `stream_id`.
* Cancellation is cooperative via `CancellationToken`.
* Ping/Pong is built in.
* Transport is pluggable.
* Memory and concurrency model strictly follows uvent coroutine scheduling.
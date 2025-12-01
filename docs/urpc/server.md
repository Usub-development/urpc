# Server Behaviour

This section describes `RpcServer`, `RpcConnection`, and `RpcMethodRegistry`.

## RpcServer

Key fields:

* `std::string host_`
* `uint16_t port_`
* `int threads_`
* `RpcMethodRegistry registry_`

### Running the server

Two entry points:

* `Awaitable<void> RpcServer::run_async()`

    * Runs the `accept_loop()` coroutine.
* `void RpcServer::run()`

    * Creates `usub::Uvent` with `threads_` worker threads.
    * Spawns `run_async()` via `system::co_spawn`.
    * Calls `uvent.run()` to start event loop.

### Accept loop

`RpcServer::accept_loop()`:

1. Creates `net::TCPServerSocket acceptor{host_.c_str(), port_}`.
2. Enters infinite loop:

    * Calls `auto soc = co_await acceptor.async_accept()`.
    * If `soc` is empty:

        * Logs warning.
        * Sleeps for 50ms via `this_coroutine::sleep_for(50ms)`.
        * Continues.
    * On success:

        * Wraps accepted socket into `std::make_shared<TcpRpcStream>(std::move(soc.value()))`.
        * Creates `std::make_shared<RpcConnection>(stream, registry_)`.
        * Spawns `RpcConnection::run_detached(conn)` via `co_spawn`.

Each accepted TCP connection gets its own `RpcConnection` instance and dedicated read loop.

## RpcMethodRegistry

`RpcMethodRegistry` maps `method_id` → handler function pointer.

Handler type:

```cpp
using RpcHandlerAwaitable = usub::uvent::task::Awaitable<void>;

using RpcHandlerFn = usub::uvent::task::Awaitable<std::vector<uint8_t>>(
    RpcContext&, std::span<const uint8_t>);

using RpcHandlerPtr = RpcHandlerFn*;
```

API:

* `void register_method(uint64_t method_id, RpcHandlerPtr fn);`
* `void register_method(std::string_view name, RpcHandlerPtr fn);`

    * Uses `fnv1a64_rt(name)` to compute `method_id`.
* `RpcHandlerPtr find(uint64_t method_id) const;`

Compile-time registration helper:

```cpp
template <uint64_t MethodId, typename F>
void RpcMethodRegistry::register_method_ct(F&& f)
{
    this->register_method(
        MethodId,
        static_cast<RpcHandlerPtr>(+std::forward<F>(f))
    );
}
```

`RpcServer` exposes the registry:

```cpp
RpcMethodRegistry& RpcServer::registry();
```

And forwarders:

```cpp
void register_method(uint64_t method_id, RpcHandlerFn fn);
void register_method(std::string_view name, RpcHandlerFn fn);

template <uint64_t MethodId, typename F>
void register_method_ct(F&& f);
```

## RpcContext

Passed to every handler:

```cpp
struct RpcContext
{
    IRpcStream& stream;
    uint32_t stream_id;
    uint64_t method_id;
    uint16_t flags;
    usub::uvent::sync::CancellationToken cancel_token;
};
```

* `stream` – underlying stream used by the connection (shared between handlers and control path).
* `stream_id` – ID of the request stream.
* `method_id` – numeric method identifier.
* `flags` – frame flags from the Request header.
* `cancel_token` – token linked to client cancellation; handlers may observe/await it.

## RpcConnection

Per-connection actor that:

* Reads frames from `IRpcStream`.
* Dispatches Requests to handlers.
* Sends Responses, errors, and Pong frames.
* Manages per-stream cancellation state.

Key fields:

* `std::shared_ptr<IRpcStream> stream_`
* `RpcMethodRegistry& registry_`
* `AsyncMutex write_mutex_`
* `AsyncMutex cancel_map_mutex_`
* `std::unordered_map<uint64_t, std::shared_ptr<CancellationSource>> cancel_map_`

### Lifecycle

`RpcConnection::run_detached(std::shared_ptr<RpcConnection> self)`:

* Ensures `self` is non-null.
* Awaits `self->loop()`.
* Returns.

### Main loop

`RpcConnection::loop()`:

1. Validates `stream_` is not null.
2. Enters infinite loop:

    * If `stream_` becomes null, exits.
    * Reads header (28 bytes) into `DynamicBuffer`.
    * Parses header with `parse_header()`.
    * Validates `magic == 0x55525043` and `version == 1`. On failure, shuts down stream and exits.
    * If `hdr.length > 0`, reads payload bytes into `frame.payload`.
    * Dispatches on `FrameType ft = static_cast<FrameType>(header.type)`:

#### Request handling

`RpcConnection::handle_request(RpcFrame frame)`:

1. Looks up `RpcHandlerPtr fn = registry_.find(method_id)`.

2. If not found:

    * Constructs temporary `RpcContext` with empty `CancellationToken`.
    * Sends error response with `error_code = 404`, `message = "Unknown method"`.
    * Returns.

3. Creates `CancellationSource src` and stores in `cancel_map_[stream_id]` under `cancel_map_mutex_`.

4. Builds `RpcContext ctx`:

    * `stream = *stream_`
    * `stream_id = header.stream_id`
    * `method_id = header.method_id`
    * `flags = header.flags`
    * `cancel_token = src->token()`

5. Constructs payload span from `frame.payload`.

6. Invokes handler coroutine: `std::vector<uint8_t> resp = co_await (*fn)(ctx, body);`.

7. Removes entry from `cancel_map_` for this `stream_id`.

8. Sends success response via `send_response(ctx, resp_span)`.

#### Cancel handling

`RpcConnection::handle_cancel(RpcFrame frame)`:

1. Looks up `CancellationSource` in `cancel_map_` for `stream_id` under `cancel_map_mutex_`.
2. If found:

    * Erases the entry.
    * Calls `src->request_cancel()`.
3. If not found: logs missing cancel source.

Handler code is expected to observe the `CancellationToken` and cooperatively stop work.

#### Ping handling

`RpcConnection::handle_ping(RpcFrame frame)`:

1. Verifies `stream_` is non-null.
2. Builds `RpcFrameHeader` for Pong:

    * `type = FrameType::Pong`
    * `flags = FLAG_END_STREAM`
    * `stream_id = frame.header.stream_id`
    * `method_id = frame.header.method_id`
    * `length = 0`
3. Sends Pong via `locked_send(hdr, {})`.

#### Unknown frame types

All frames other than `Request`, `Cancel`, `Ping` are logged and ignored.

### Sending frames from server

`RpcConnection::locked_send(const RpcFrameHeader& hdr, std::span<const uint8_t> body)`:

* Acquires `write_mutex_`.
* Calls `send_frame(*stream_, hdr, body)` (see transport section).
* Ensures frames from different coroutines are not interleaved.

`RpcConnection::send_response(RpcContext& ctx, std::span<const uint8_t> body)`:

* Builds `Response` header:

    * `type = FrameType::Response`
    * `flags = FLAG_END_STREAM`
    * `stream_id = ctx.stream_id`
    * `method_id = ctx.method_id`
    * `length = body.size()`

* Uses `locked_send` to send.

`RpcConnection::send_simple_error(...)` builds error payload and sends `Response` with `FLAG_ERROR | FLAG_END_STREAM`.
# Server Behaviour

This section describes how the server side of uRPC operates:
`RpcServer`, `RpcConnection`, and `RpcMethodRegistry`.

It covers:

* TCP / TLS / mTLS server transports
* Accept loop
* Per-connection RPC handling
* Method registry
* **String-returning handlers**
* Cancellation
* Response/error handling
* Behaviour when app-level AES is enabled

---

# **Important encryption note**

The uRPC **28-byte header is never encrypted**.

* TLS/mTLS encrypts the transport stream, but the header remains in plaintext at the uRPC framing layer.
* App-level AES (FLAG_ENCRYPTED) **only encrypts the payload**, never the header.

The server always receives the header unencrypted and can read all fields before decrypting payload (if needed).

---

# **RpcServer**

`RpcServer` owns:

* Listener address
* Thread count
* Method registry
* Transport factory (TCP / TLS / mTLS)
* Optional cancel-observability callback

```cpp
struct RpcServerConfig {
    std::string host;
    uint16_t    port;
    int         threads = 1;
    std::shared_ptr<IRpcStreamFactory> stream_factory;

    // Optional. Invoked whenever the server drops a request because
    // the client sent a Cancel frame for it. See "Cancellation" below.
    RpcCancelCallback on_request_cancelled;
};
```

If `stream_factory` is null, the server uses plain TCP (`TcpRpcStreamFactory`).
If `on_request_cancelled` is null, cancellations are still honoured — the
handler simply runs to completion and its response is dropped — but the server
will not notify you about it.

---

# **Registering Methods**

A handler normally returns:

```cpp
Awaitable<std::vector<uint8_t>>
```

But the server also supports **returning `std::string` directly**, which is automatically re-encoded as binary payload.

## **Binary-returning handler**

```cpp
server.register_method_ct<method_id("User.GetRaw")>(
    [](RpcContext&, std::span<const uint8_t> body)
        -> Awaitable<std::vector<uint8_t>>
    {
        co_return std::vector<uint8_t>{1,2,3,4};
    });
```

## **String-returning handler**

```cpp
server.register_method_ct<method_id("Example.String")>(
    [](RpcContext&, std::span<const uint8_t> body)
        -> Awaitable<std::string>
    {
        std::string in((char*)body.data(), body.size());
        co_return "echo: " + in;
    });
```

The server translates it into:

```
vector<uint8_t> payload = bytes(string)
```

No additional framing or encoding is applied.

This makes API development significantly simpler, especially for JSON/Glaze/YAML-based servers.

---

# **When to use string-returning handlers**

### ✔ Good when:

* returning small or medium textual responses
* producing JSON/Glaze/YAML payloads
* you want minimal boilerplate

### ✘ Avoid when:

* returning binary blobs
* returning large serialized structures
* you need zero allocations (use `vector<uint8_t>` directly)

---

# **Running the server**

### Asynchronous:

```cpp
Awaitable<void> RpcServer::run_async();
```

### Synchronous:

```cpp
void RpcServer::run();
```

Creates `Uvent` with N threads and starts the accept loop.

---

# **Accept Loop**

Standard accept+spawn design:

* Accept TCP
* Wrap in TLS (if enabled)
* Create `RpcConnection`
* Spawn its coroutine

Each connection executes independently.

---

# **RpcMethodRegistry**

Maps:

```
method_id → RpcHandlerFn
```

Supports:

* compile-time registration (`register_method_ct`)
* runtime registration (`register_method`)

String-returning functors are wrapped automatically.

---

# **RpcConnection**

Each TCP/TLS connection has exactly one RpcConnection.

Responsibilities:

* read frames
* parse 28-byte header
* optional AES-GCM decrypt
* call registered handler
* build response
* write locked frame
* manage cancellation
* handle ping/pong

---

# **Request Handling**

Handler chain:

```
frame → lookup → RpcContext → functor(ctx, body) → {string|binary} → response frame
```

If handler returns `std::string` → server converts to raw bytes.

---

# **Response Encoding**

### Vector handler:

```
std::vector<uint8_t> payload → raw bytes unchanged
```

### String handler:

```
std::string s → vector<uint8_t>(s.begin(), s.end())
```

Flags always include:

```
END_STREAM
```

Unless error or cancellation.

---

# **Cancellation**

A client can cancel an in-flight RPC at any time by sending a `Cancel` frame
with the matching `stream_id`. Cancellation can reach the server in three
places, and uRPC observes it in all of them:

1. **Before the handler starts.** If the cancel frame arrives and is processed
   before the handler coroutine is spawned, the server skips invocation
   entirely.

2. **During handler execution.** The handler can cooperatively check
   cancellation through its `RpcContext`:

   ```cpp
   server.register_method_ct<method_id("Slow.Work")>(
       [](RpcContext& ctx, std::span<const uint8_t> body)
           -> Awaitable<std::vector<uint8_t>>
       {
           for (int i = 0; i < N; ++i) {
               if (ctx.cancel_token.stop_requested())
                   co_return std::vector<uint8_t>{};
               co_await do_chunk(i);
           }
           co_return build_result();
       });
   ```

   `ctx.cancel_token.stop_requested()` is a non-blocking check; there is also
   `ctx.cancel_token.on_cancel()` for coroutines that want to `co_await`
   cancellation directly.

3. **After the handler finishes, before the response is sent.** If the
   cancel arrived while the handler was still producing its result, the
   server checks one more time after the handler returns and drops the
   response instead of transmitting it.

In all three cases, the request is discarded silently — no error frame is
sent to the client — because the client has already indicated it no longer
cares about the result.

Request handlers are spawned via `co_spawn` rather than `co_await`ed inline,
so the per-connection read loop stays responsive to further frames
(including `Cancel` and `Ping`) while handlers run. A slow handler no longer
blocks the reader.

## Observing cancellation from the server

Because cancellation is silent by default, you may want visibility into how
often and where it happens — for metrics, tracing, or cost accounting. That
is what `RpcServerConfig::on_request_cancelled` is for:

```cpp
enum class RpcCancelStage : uint8_t
{
    BeforeHandler = 0,  // cancel observed before handler ran
    AfterHandler  = 1,  // cancel observed after handler produced a result
};

struct RpcCancelEvent
{
    RpcCancelStage stage;
    uint32_t       stream_id;
    uint64_t       method_id;
    size_t         dropped_response_bytes;  // 0 for BeforeHandler
};

using RpcCancelCallback = std::function<void(const RpcCancelEvent&)>;
```

Wiring it up:

```cpp
urpc::RpcServerConfig cfg{
    .host = "0.0.0.0",
    .port = 45900,
    .threads = 4,
    .on_request_cancelled = [](const RpcCancelEvent& ev) {
        metrics::cancelled_rpcs.increment(
            {{"stage", ev.stage == RpcCancelStage::BeforeHandler
                       ? "before" : "after"},
             {"method", std::to_string(ev.method_id)}});

        if (ev.stage == RpcCancelStage::AfterHandler)
            metrics::wasted_response_bytes.add(
                ev.dropped_response_bytes);
    },
};
```

### Callback rules

* Invoked exactly once per cancellation.
* Runs on an uvent worker thread. If your callback must touch thread-local
  state, keep that in mind.
* Must not throw. An exception escaping the callback is a programming error;
  the server treats it as fatal.
* Must not block. It is called inline in the read loop; expensive work
  should be offloaded to a queue.
* `stage == BeforeHandler`: `dropped_response_bytes` is always 0 (no
  response was produced).
* `stage == AfterHandler`: `dropped_response_bytes` is the size of the
  encoded response body that would have been sent to the client.

## Three-level cancellation model

To summarise, the cancellation story has three layers, and you opt into as
many as you want:

| Layer                  | Who is responsible     | What it gives you                        |
|------------------------|------------------------|------------------------------------------|
| In-handler checks      | Handler code           | Stops doing work inside your own logic.  |
| Library pre/post check | Server library         | Skips handler invocation or response.    |
| Cancel-aware awaiters  | uvent primitives       | *Not yet implemented.*                   |

The first two are always active. The third — cancellation that interrupts a
handler suspended inside a downstream `co_await` (DB query, outbound RPC,
etc.) — would require the underlying awaiters to observe `cancel_token`.
That is an upstream change in uvent's synchronisation primitives and is not
part of the current release.

---

# **Error Handling**

Errors produce:

```
type  = RESPONSE
flags = END_STREAM | ERROR
payload = [u32 code][u32 msg_len][msg bytes]
```

String-returning handlers can also throw — errors propagate normally.

---

# **TLS / mTLS on the server**

TLS encrypts transport.
Headers stay plaintext at uRPC layer (after TLS decrypt).

mTLS populates:

```cpp
ctx.peer->authenticated
ctx.peer->common_name
ctx.peer->subject
```

This works the same for string and binary handlers.

---

# **App-level AES (payload encryption)**

If enabled:

* only payload is encrypted
* header is not
* symmetric AES-GCM key is derived via:

```
SSL_export_keying_material("urpc_app_key_v1")
```

Server decrypts before passing to handler.

String-returning handlers still work transparently.

### Fail-closed

The server never falls back to plaintext. If encryption of an outbound
response (or error frame) fails — missing key, cipher context failure,
etc. — the server does not send the frame in clear; it tears down the
affected connection instead. The client's reader loop will observe the
drop and fail any pending calls on that connection. This guarantees that
a connection which negotiated AES-GCM will never emit an un-encrypted
frame during its lifetime.

The same rule applies to inbound decryption failures: a frame marked
`FLAG_ENCRYPTED` whose GCM tag does not verify is treated as a
connection-level error.

---

# **Summary**

* Server supports binary and string-returning handlers.
* String-returning functions are auto-wrapped into binary uRPC responses.
* Header is always plaintext at uRPC level (TLS decrypts first).
* Payload may be AES-encrypted depending on flags; encryption is fail-closed.
* Per-connection coroutine model ensures scalability; handlers run detached
  from the read loop so cancel/ping frames remain responsive under load.
* Cancellation is observed before and after handler execution, and optionally
  reported through `RpcServerConfig::on_request_cancelled`.
* Registry maps method IDs to handlers of either type.
* uRPC server fully supports TCP, TLS, and mTLS.
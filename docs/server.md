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

---

# **Summary**

* Server supports binary and string-returning handlers.
* String-returning functions are auto-wrapped into binary uRPC responses.
* Header is always plaintext at uRPC level (TLS decrypts first).
* Payload may be AES-encrypted depending on flags.
* Per-connection coroutine model ensures scalability.
* Registry maps method IDs to handlers of either type.
* uRPC server fully supports TCP, TLS, and mTLS.
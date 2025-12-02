# Transport Abstraction and Method IDs

uRPC is transport-agnostic.  
All framing works on top of a minimal asynchronous byte-stream interface.

The transport layer is responsible for:

- async reads/writes
- connection shutdown
- optional TLS/mTLS handshake
- optional certificate validation

The protocol itself does **not** depend on TCP or OpenSSL directly.

---

# IRpcStream

The core abstraction:

```cpp
struct IRpcStream
{
    virtual usub::uvent::task::Awaitable<ssize_t> async_read(
        usub::uvent::utils::DynamicBuffer& buf,
        size_t max_read
    ) = 0;

    virtual usub::uvent::task::Awaitable<ssize_t> async_write(
        uint8_t* data,
        size_t len
    ) = 0;

    virtual void shutdown() = 0;

    // Optional; not all transports implement it
    virtual PeerCertInfo peer_certificate() const { return {}; }

    virtual ~IRpcStream() = default;
};
```

### Semantics

* `async_read(buf, max_read)`

    * Appends **up to `max_read` bytes** into `buf`.
    * Returns:
      `> 0` — number of bytes appended
      `<= 0` — EOF or error.

* `async_write(data, len)`

    * Writes exactly `len` bytes.
    * Returns:
      `> 0` — written bytes
      `<= 0` — error.

* `shutdown()`

    * Terminates the transport (close TCP, close TLS session, etc).

The uRPC layer (client/server) uses only these three operations.

---

# IRpcStreamFactory

Both client and server obtain streams through a factory:

```cpp
struct IRpcStreamFactory
{
    virtual std::shared_ptr<IRpcStream>
    create_client_stream(std::string host, uint16_t port) = 0;

    virtual std::shared_ptr<IRpcStream>
    create_server_stream(usub::uvent::net::TCPClientSocket&& sock) = 0;

    virtual ~IRpcStreamFactory() = default;
};
```

Factories produce:

* plain TCP streams (`TcpRpcStream`);
* TLS/mTLS streams (`TlsRpcStream`);
* custom transports (e.g. QUIC in the future).

---

# TcpRpcStream

A thin wrapper over `TCPClientSocket`:

```cpp
class TcpRpcStream : public IRpcStream
{
public:
    explicit TcpRpcStream(usub::uvent::net::TCPClientSocket&& sock);

    Awaitable<ssize_t> async_read(DynamicBuffer& buf, size_t max_read) override;
    Awaitable<ssize_t> async_write(uint8_t* data, size_t len) override;
    void shutdown() override;

private:
    usub::uvent::net::TCPClientSocket socket_;
};
```

Properties:

* No buffering, no framing.
* Direct passthrough to uvent socket primitives.
* `shutdown()` just closes FD.

---

# TlsRpcStream (TLS / mTLS)

TLS transport is a drop-in implementation of `IRpcStream`, using OpenSSL in non-blocking mode and uvent coroutines.

Provided by:

```cpp
class TlsRpcStreamFactory : public IRpcStreamFactory;
class TlsRpcStream        : public IRpcStream;
```

### Client-side flow

`create_client_stream(host, port)`:

1. Create TCP socket.
2. Connect via `async_connect`.
3. Initialize OpenSSL SSL object.
4. Configure CA, certificate, private key, server-name (SNI).
5. Perform async TLS handshake (loop around SSL_ERROR_WANT_READ/WRITE).
6. Return ready TLS stream.

### Server-side flow

`create_server_stream(TCPClientSocket&& sock)`:

1. Attach SSL to accepted socket.
2. Load server certificate and key.
3. If `require_client_cert == true` → enable mTLS.
4. Perform async handshake.
5. Verify client certificate (if mTLS).
6. Return ready TLS stream.

### Peer certificate

Server and client may access peer certificate:

```cpp
auto info = stream->peer_certificate();
info.subject;
info.issuer;
info.san_dns;
info.raw_der;
```

Used for mTLS authorization, logging, etc.

---

# IO helpers: write_all and send_frame

Defined in `IOOps.h`.

```cpp
inline task::Awaitable<bool> write_all(
    IRpcStream& stream,
    const uint8_t* data,
    size_t n);

inline task::Awaitable<bool> send_frame(
    IRpcStream& stream,
    const RpcFrameHeader& hdr,
    std::span<const uint8_t> payload);
```

### `write_all`

* Performs a **single write** call.
* Returns `false` on `<= 0`.

(Safe because uRPC frames are small and under a mutex. The transport already guarantees full-write or error semantics.)

### `send_frame`

1. Serializes header into local 32-byte array.
2. Calls `write_all` on header.
3. Calls `write_all` on payload (if any).

Used by:

* `RpcClient` (`async_call`, `async_ping`, responses to server pings)
* `RpcConnection` (`send_response`, errors, `handle_ping`)

---

# Method IDs

Method IDs are **64-bit FNV-1a** hashes of method names.

Why:

* Fast
* Deterministic
* Low collision probability
* Compile-time viable

All requests use:

```
method_id: u64
```

### Runtime hashing

```cpp
uint64_t fnv1a64_rt(std::string_view s) noexcept;
```

Used by:

* `rpcServer.register_method("name", fn)`
* `client->async_call("name", body)`

### Compile-time hashing

```cpp
template <size_t N>
consteval uint64_t fnv1a64_ct(const char (&str)[N]);

template <size_t N>
consteval uint64_t method_id(const char (&str)[N]);
```

Used by:

```cpp
static constexpr uint64_t EchoId = urpc::method_id("Example.Echo");

server.register_method_ct<EchoId>(handler);
co_await client->async_call_ct<EchoId>(payload);
```

Compile-time IDs guarantee:

* fully static value
* zero allocations
* no runtime hashing

---

# Summary

Transport layer guarantees:

* async read/write
* clean shutdown
* optional TLS/mTLS
* peer-certificate access
* no assumption about framing or semantics

Protocol layer guarantees:

* header serialization
* payload integrity
* method-id hashing
* multiplexing by stream id

Client and server exchange only:

```
IRpcStream::async_read / async_write
IRpcStream::shutdown()
```

Everything else (framing, method dispatch, errors, pings) lives entirely above the transport.
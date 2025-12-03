# Transport Abstraction and Method IDs

uRPC is transport-agnostic.  
All framing works on top of a minimal asynchronous byte-stream interface.

The transport layer is responsible for:

- async reads/writes
- connection shutdown
- optional TLS/mTLS handshake
- optional certificate validation
- optional app-level AES payload encryption (only payload, never header)

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
* custom transports (QUIC, pipes, etc.).

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

TLS transport is a drop-in implementation of `IRpcStream`, using OpenSSL in non-blocking mode with uvent coroutines.

Provided by:

```cpp
class TlsRpcStreamFactory : public IRpcStreamFactory;
class TlsRpcStream        : public IRpcStream;
```

### Client-side flow

`create_client_stream(host, port)`:

1. Create TCP socket.
2. Connect via `async_connect`.
3. Initialize SSL object.
4. Configure CA, certificate, key, server-name (SNI).
5. Perform async TLS handshake.
6. Derive AES exporter keys (if app-level encryption is enabled).
7. Return TLS stream.

### Server-side flow

`create_server_stream(TCPClientSocket&& sock)`:

1. Wrap socket with SSL.
2. Load server cert/key.
3. Enable mTLS if required.
4. Perform async handshake.
5. Verify client certificate if mTLS.
6. Derive AES exporter keys (if enabled).
7. Return stream.

### Header visibility under TLS

* TLS encrypts the **transport** stream.
* The **uRPC header (28 bytes) is still parsed as plaintext**, because OpenSSL
  decrypts it before delivering bytes to `async_read`.

### Peer certificate

```cpp
auto info = stream->peer_certificate();
```

Used for authorization, logging, auditing.

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

### AES note

If app-level AES is enabled:

* encryption happens **before** calling `send_frame`.
* the payload passed to `send_frame` is already:

```
IV[12] + ciphertext[...] + TAG[16]
```

`send_frame` itself is unaware of encryption.

### `write_all`

* Performs a **single write** call.
* Returns `false` on `<= 0`.

### `send_frame`

1. Serializes the 28-byte header.
2. Calls `write_all` for header.
3. Calls `write_all` for payload (raw or AES-encrypted).

Used by:

* `RpcClient`
* `RpcConnection`

---

# Method IDs

Method IDs are **64-bit FNV-1a** hashes of method names.

Why:

* deterministic
* fast
* collision probability extremely low
* trivial compile-time evaluation

### Runtime hashing

```cpp
uint64_t fnv1a64_rt(std::string_view s) noexcept;
```

### Compile-time hashing

```cpp
template <size_t N>
consteval uint64_t method_id(const char (&str)[N]);
```

Used by:

```cpp
server.register_method_ct<method_id("Example.Echo")>(fn);
client->async_call_ct<method_id("Example.Echo")>(payload);
```

---

# Summary

Transport layer provides:

* async read/write
* clean shutdown
* optional TLS/mTLS
* optional per-connection AES key derivation
* peer certificate access

Protocol layer provides:

* 28-byte plaintext header (always)
* optional payload encryption (AES-256-GCM)
* multiplexing via stream IDs
* method resolution via 64-bit FNV-1a hashes

Client and server exchange only three transport operations:

```
async_read
async_write
shutdown
```

Everything else—framing, flags, AES, errors, pings—lives fully above the transport.
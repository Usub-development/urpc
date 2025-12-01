# Transport Abstraction and Method IDs

## IRpcStream

The protocol is defined on top of an abstract async byte stream:

```cpp
struct IRpcStream
{
    virtual usub::uvent::task::Awaitable<ssize_t> async_read(
    usub::uvent::utils::DynamicBuffer& buf, size_t max_read) = 0;

    virtual usub::uvent::task::Awaitable<ssize_t> async_write(
    uint8_t* data, size_t len) = 0;

    virtual void shutdown() = 0;
    virtual ~IRpcStream() = default;
};
```

Responsibilities:

* `async_read(buf, max_read)`:

* Appends up to `max_read` bytes into `buf`.
* Returns number of bytes read or `<= 0` on EOF/error.
* `async_write(data, len)`:

* Writes up to `len` bytes from `data`.
* Returns number of bytes written or `<= 0` on error.
* `shutdown()`:

* Closes the underlying transport.

The protocol itself only relies on these operations; it is not tied to TCP specifically.

## TcpRpcStream

Concrete TCP implementation:

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

* Delegates `async_read` and `async_write` to `TCPClientSocket`.
* `shutdown()` closes the underlying socket.

## IO operations helpers

Defined in `IOOps.h`:

```cpp
inline task::Awaitable<bool> write_all(
IRpcStream& stream, const uint8_t* data, size_t n);

inline task::Awaitable<bool> send_frame(
IRpcStream& stream,
const RpcFrameHeader& hdr,
std::span<const uint8_t> payload);
```

* `write_all`:

* Calls `stream.async_write(data, n)`.
* Returns `false` on `r <= 0`, `true` otherwise.
* (Current implementation performs a single write; callers normally pass small buffers.)
* `send_frame`:

* Serializes `RpcFrameHeader` into a fixed buffer using `serialize_header`.
* Writes the header via `write_all`.
* Writes payload (if non-empty) via `write_all`.

Both client and server use `send_frame` under their respective write mutexes.

## Method IDs and hashing

Method IDs are 64-bit unsigned integers. By default they are computed using 64-bit FNV-1a over the method name.

Helpers in `Hash.h`:

```cpp
constexpr uint64_t fnv1a64_rt(std::string_view s) noexcept;
template <size_t N>
consteval uint64_t fnv1a64_ct(const char (&str)[N]);

template <size_t N>
consteval uint64_t method_id(const char (&str)[N])
{
    return fnv1a64_ct(str);
}
```

Usage patterns:

### Runtime IDs

* Server:

```cpp
rpcServer.register_method("echo", &echo_handler);
```

This computes `method_id = fnv1a64_rt("echo")` and stores handler under that ID.

* Client:

```cpp
co_await client->async_call("echo", request_body);
```

This computes `method_id` the same way on the client side.

### Compile-time IDs

* Server:

```cpp
constexpr uint64_t EchoId = urpc::method_id("echo");

rpcServer.register_method_ct<EchoId>(&echo_handler);
```

* Client:

```cpp
constexpr uint64_t EchoId = urpc::method_id("echo");

co_await client->async_call_ct<EchoId>(request_body);
```

Compile-time IDs guarantee that the numeric value is known at compile time and consistent between client and server, as long as the method name string is identical.
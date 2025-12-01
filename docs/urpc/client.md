# Client Behaviour

This section describes how `RpcClient` uses the wire protocol.

## RpcClient structure

Key members:

* `std::shared_ptr<IRpcStream> stream_` – active transport.
* `std::atomic<uint32_t> next_stream_id_{1}` – stream ID allocator.
* `std::atomic<bool> running_{false}` – reader loop control flag.
* `AsyncMutex write_mutex_` – serialize writes.
* `AsyncMutex connect_mutex_` – serialize connect attempts.
* `AsyncMutex pending_mutex_` – protect `pending_calls_`.
* `AsyncMutex ping_mutex_` – protect `ping_waiters_`.
* `std::unordered_map<uint32_t, std::shared_ptr<PendingCall>> pending_calls_`
* `std::unordered_map<uint32_t, std::shared_ptr<AsyncEvent>> ping_waiters_`

`PendingCall`:

```cpp
struct PendingCall
{
    std::shared_ptr<AsyncEvent> event;
    std::vector<uint8_t> response;
    bool     error{false};
    uint32_t error_code{0};
    std::string error_message;
};
```

## Establishing a connection

`RpcClient::ensure_connected()`:

1. If `stream_` is non-null and `running_ == true`, it returns `true`.
2. Acquires `connect_mutex_` to avoid concurrent connect attempts.
3. Creates a `net::TCPClientSocket`.
4. Calls `async_connect(host_, port_)`.
5. On success:

* Wraps the socket in `TcpRpcStream`.
* Stores it into `stream_`.
* Sets `running_ = true`.
* Spawns the `reader_loop()` coroutine via `system::co_spawn`.

If `async_connect` fails, `ensure_connected()` returns `false`.

## Sending a request

`RpcClient::async_call(uint64_t method_id, std::span<const uint8_t> request_body)`:

1. Calls `ensure_connected()`. On failure returns an empty vector.
2. Allocates a new `stream_id`:

* `sid = next_stream_id_.fetch_add(1)`, skipping `0`.

3. Creates a `PendingCall`:

* With `AsyncEvent` (manual reset, initially unsignalled).
* Inserts it into `pending_calls_[sid]` under `pending_mutex_`.

4. Builds `RpcFrameHeader`:

* `magic = 0x55525043`
* `version = 1`
* `type = FrameType::Request`
* `flags = FLAG_END_STREAM`
* `stream_id = sid`
* `method_id = method_id`
* `length = request_body.size()`

5. Acquires `write_mutex_`.
6. Copies `stream_` to a local shared pointer and verifies it’s non-null.
7. Sends the frame using `send_frame(*stream, hdr, request_body)`.

* On send failure: removes `pending_calls_[sid]` and returns empty vector.

8. Waits on `call->event->wait()`.
9. After wakeup:

* Removes `pending_calls_[sid]` under `pending_mutex_`.
* If `call->error == true`, returns empty vector.
* Otherwise returns `call->response`.

### Name-based and compile-time method IDs

Helpers:

```cpp
template <size_t N>
Awaitable<std::vector<uint8_t>> async_call(
const char (&name)[N],
std::span<const uint8_t> request_body);

template <uint64_t MethodId>
Awaitable<std::vector<uint8_t>> async_call_ct(
std::span<const uint8_t> request_body);
```

* `async_call(name, ...)`:

* Computes `method_id = fnv1a64_rt(std::string_view{name, N - 1})` at runtime.
* `async_call_ct<MethodId>(...)`:

* Uses a compile-time constant `MethodId` (usually produced by `method_id("...")`).

## Reader loop

`RpcClient::reader_loop()`:

1. Runs while `running_ == true`.
2. Captures `stream_` into local variable; if null, exits.
3. Reads header:

* Calls `read_exact(*stream, head_buffer, sizeof(RpcFrameHeader))`.
* On failure or wrong size, exits loop.

4. Parses header bytes using `parse_header()`.

* Validates `magic` and `version`. On mismatch, exits.

5. If `hdr.length > 0`, reads payload into `frame.payload` with `read_exact`.

* On failure or size mismatch, exits.

6. Dispatches by `FrameType`:

### Handling Response

* Looks up `PendingCall` by `frame.header.stream_id` under `pending_mutex_`.
* If not found, logs a warning and ignores.
* Checks `FLAG_ERROR`:

* If set:

* Calls `parse_error_payload(frame.payload, code, msg)`.
* Populates `call->error`, `call->error_code`, `call->error_message`.
* If not set:

* Copies payload bytes into `call->response`.
* Sets `call->error = false`.
* Signals `call->event->set()`.

### Handling Ping

* Builds a `Pong` response:

* Same `stream_id` and `method_id`.
* `type = FrameType::Pong`
* `flags = FLAG_END_STREAM`
* `length = 0`

* Under `write_mutex_`, sends the pong via `send_frame`.

### Handling Pong

* Looks up `AsyncEvent` in `ping_waiters_` by `stream_id` under `ping_mutex_`.
* If found, calls `evt->set()`.

### Other types

* `Request`, `Stream`, `Cancel` and unknown types from the server are logged and ignored by the client.

### Loop termination

On exit from the loop:

* Sets `running_ = false`.
* Under `pending_mutex_`:

* Marks all pending calls as errored with `"Connection closed"`.
* Sets their events.
* Clears `pending_calls_`.
* Under `ping_mutex_`:

* Sets all pending ping events.
* Clears `ping_waiters_`.
* Under `connect_mutex_`:

* Resets `stream_` to `nullptr`.

## Ping / Pong API

`RpcClient::async_ping()`:

1. Calls `ensure_connected()`. On failure returns `false`.

2. Allocates `sid` using `next_stream_id_`.

3. Creates `AsyncEvent`, inserts into `ping_waiters_[sid]` under `ping_mutex_`.

4. Builds header:

* `type = FrameType::Ping`
* `flags = FLAG_END_STREAM`
* `stream_id = sid`
* `method_id = 0`
* `length = 0`

5. Sends frame under `write_mutex_`.

6. Waits on `evt->wait()`.

7. Under `ping_mutex_`, checks if `ping_waiters_` still contains `sid`.

* If yes: removes it and returns `true`.
* Otherwise returns `false`.

## Closing the client

`RpcClient::close()`:

* Sets `running_ = false`.
* Swaps out `stream_` into a local variable and calls `stream->shutdown()` if present.
* The reader loop will eventually exit and perform cleanup described above.
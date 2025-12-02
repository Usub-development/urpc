# Client Behaviour

This section describes how `RpcClient` works, including TCP, TLS, and mTLS modes.

---

# Overview

`RpcClient` is a fully asynchronous, multiplexed RPC client built on top of `uvent`.  
Transport is abstracted via the `IRpcStream` interface, so the client supports:

- **TCP** (`TcpRpcStream`)
- **TLS** (`TlsRpcStream`)
- **mTLS** (mutual TLS)

Transport is selected through `RpcClientConfig.stream_factory`.

---

# Internal Structure

Key members:

* `std::shared_ptr<IRpcStream> stream_` – active transport (TCP/TLS/mTLS).
* `std::atomic<uint32_t> next_stream_id_{1}` – stream ID allocator.
* `std::atomic<bool> running_{false}` – reader loop flag.
* `AsyncMutex write_mutex_` – serialize writes.
* `AsyncMutex connect_mutex_` – serialize connects.
* `AsyncMutex pending_mutex_` – protect RPC calls map.
* `AsyncMutex ping_mutex_` – protect ping waiters.
* `unordered_map<uint32_t, shared_ptr<PendingCall>> pending_calls_`
* `unordered_map<uint32_t, shared_ptr<AsyncEvent>> ping_waiters_`

`PendingCall`:

```cpp
struct PendingCall {
    std::shared_ptr<AsyncEvent> event;
    std::vector<uint8_t> response;
    bool        error{false};
    uint32_t    error_code{0};
    std::string error_message;
};
```

---

# Connection Establishment

`RpcClient::ensure_connected()`:

1. If already connected (`stream_ != nullptr` and `running_ == true`)
   → returns `true`.

2. Locks `connect_mutex_`.

3. Creates a stream via:

```
config.stream_factory->create_client_stream(host, port)
```

Depending on factory:

| Factory               | Transport |
|-----------------------|-----------|
| `TcpRpcStreamFactory` | TCP       |
| `TlsRpcStreamFactory` | TLS/mTLS  |

4. On success:

* `stream_ = created stream`
* `running_ = true`
* spawns `reader_loop()` via `co_spawn`

5. On failure: returns `false`.

For TLS/mTLS:

* handshake happens **inside** `TlsRpcStream`
* certificate verification is performed according to config

---

# Sending Requests

`RpcClient::async_call(method_id, request_body)`:

1. `ensure_connected()`

    * if fails → returns empty vector

2. Allocates new non-zero `stream_id`.

3. Creates `PendingCall` with `AsyncEvent` and inserts it under `pending_mutex_`.

4. Builds `RpcFrameHeader`.

5. Locks `write_mutex_` and sends frame via:

```
send_frame(*stream_, header, request_body)
```

6. Waits on `call->event->wait()`.

7. Removes entry from `pending_calls_`.

8. Returns either:

    * `call->response`
    * empty vector if `call->error == true`

## Name-based and compile-time helpers

```cpp
client->async_call("Service.Method", body);
client->async_call_ct<method_id("Service.Method")>(body);
```

* First computes hash at runtime.
* Second uses compile-time hash.

---

# Reader Loop

`RpcClient::reader_loop()` continuously:

1. Captures `stream_`.
   If null → exit.

2. Reads header (**28 bytes**) via `read_exact`.
   EOF or error → exit.

3. Parses header.
   Invalid magic/version → exit.

4. Reads payload (`hdr.length > 0`).

5. Dispatches by `FrameType`:

---

## Response Frames

* Lookup `PendingCall` by `stream_id`.
* If not found → ignore.
* If `FLAG_ERROR`:

    * decode error payload
    * set `call->error = true`
    * copy `error_code`, `error_message`
* Else:

    * copy payload into `call->response`
* Trigger `call->event->set()`.

---

## Ping Frames

Client never receives application-level pings in normal flow, but supports it:

* Build a Pong
* Send under `write_mutex_`

---

## Pong Frames

* Lookup waiter in `ping_waiters_`
* If found → trigger event

---

## Unknown or server-side frames

* Logged
* Ignored

---

# Loop Termination

When the loop exits:

1. `running_ = false`

2. For all pending RPCs:

    * mark error `"Connection closed"`
    * trigger events

3. For all ping waiters:

    * trigger events

4. Reset `stream_ = nullptr` under `connect_mutex_`

This guarantees that all awaiting coroutines wake up.

---

# Ping / Pong

`async_ping()` is a lightweight API-level liveness probe:

1. `ensure_connected()`
2. Allocate `sid`
3. Insert `AsyncEvent` into `ping_waiters_`
4. Send Ping frame
5. Wait on event
6. If waiter still exists → success

Useful for:

* keep-alive
* connection warm-up
* readiness checks

---

# Closing the Client

`RpcClient::close()`:

* sets `running_ = false`
* swaps out `stream_`
* calls `stream->shutdown()`
* reader loop terminates and performs cleanup

This is a **graceful** shutdown, not an error.

---

# TLS and mTLS Support

TLS is activated by selecting `TlsRpcStreamFactory`:

```cpp
urpc::RpcClientConfig cfg;
cfg.host = "server";
cfg.port = 45900;

urpc::TlsClientConfig tls;
tls.enable = true;
tls.verify_peer = true;
tls.ca_file = "ca.crt";
tls.cert_file = "client.crt"; // optional
tls.key_file = "client.key";   // optional
tls.server_name = "localhost"; // SNI

cfg.stream_factory = std::make_shared<TlsRpcStreamFactory>(tls);

RpcClient client{cfg};
```

mTLS requires:

* `verify_peer = true`
* client certificate + key

The rest of the client logic is identical.

---

# CLI Tool

The repository includes a simple CLI client (`urpc_cli`) using the same API.

Examples:

### TCP

```
urpc_cli --host 127.0.0.1 --port 45900 \
         --method Example.Echo \
         --data "hello"
```

### TLS

```
urpc_cli --tls --tls-ca ca.crt \
         --tls-server-name localhost \
         --host 127.0.0.1 --port 45900 \
         --method Example.Echo --data "hello"
```

### mTLS

```
urpc_cli --tls \
         --tls-ca ca.crt \
         --tls-cert client.crt \
         --tls-key client.key \
         --tls-server-name localhost \
         --host 127.0.0.1 --port 45900 \
         --method Example.Echo --data "hello secure"
```

The CLI behaves exactly like `RpcClient`.
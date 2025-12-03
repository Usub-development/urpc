# Client Behaviour

This section describes how `RpcClient` works, including TCP, TLS, mTLS modes
and optional app-level AES encryption.

---

# Encryption note

From the client’s point of view:

* The **28-byte uRPC header is never encrypted** by the protocol itself.
* TLS/mTLS encrypt the TCP stream, but the header is still parsed as a normal
  28-byte structure (magic/version/type/flags/stream_id/method_id/length).
* App-level AES (when enabled, `FLAG_ENCRYPTED` set) encrypts **only the payload**:
  `IV[12] + ciphertext[...] + TAG[16]`.

The client always:

1. Reads and parses the header in plaintext.
2. If `FLAG_ENCRYPTED` is set, decrypts the payload with AES-256-GCM using a key
   derived from the TLS exporter.
3. Passes the decrypted body to user code.

---

# Overview

`RpcClient` is a fully asynchronous, multiplexed RPC client built on top of `uvent`.  
Transport is abstracted via the `IRpcStream` interface, so the client supports:

- **TCP** (`TcpRpcStream`)
- **TLS** (`TlsRpcStream`)
- **mTLS** (mutual TLS)
- Optional app-level AES (per-connection key from TLS exporter)

Transport (and whether TLS is used) is selected through
`RpcClientConfig.stream_factory`.

Whether app-level AES is used is controlled by the TLS-side configuration
(shared between client and server).

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

```cpp
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
* optionally spawns `ping_loop()` if ping interval is configured

5. On failure: returns `false`.

For TLS/mTLS:

* TLS handshake happens **inside** `TlsRpcStream`.
* Certificate verification is performed according to `TlsClientConfig`
  (CA, hostname, client cert for mTLS, etc.).
* If app-level AES is enabled, the per-connection AES key is derived from the
  TLS exporter during/after the handshake and bound to that stream.

---

# Sending Requests

`RpcClient::async_call(method_id, request_body)`:

1. Calls `ensure_connected()`.

    * If it fails → returns empty vector (no request is sent).

2. Allocates a new non-zero `stream_id`.

3. Creates `PendingCall` with `AsyncEvent` and inserts it under `pending_mutex_`.

4. Builds `RpcFrameHeader`:

    * `type = FrameType::Request`
    * `flags` includes `FLAG_END_STREAM`
    * MAY also include:

        * `FLAG_TLS` / `FLAG_MTLS` (depending on transport)
        * `FLAG_ENCRYPTED` if app-level AES is enabled (payload will be AES-256-GCM)

5. Locks `write_mutex_` and sends the frame:

```cpp
send_frame(*stream_, header, request_body_or_ciphertext);
```

If `FLAG_ENCRYPTED` is used, the client encrypts:

```text
plaintext_body → IV[12] + ciphertext + TAG[16]
```

and passes that as the payload.

6. Waits on `call->event->wait()`.

7. Removes entry from `pending_calls_`.

8. Returns:

* decrypted `call->response` for success
* empty vector if `call->error == true` or on connection failure

## Name-based and compile-time helpers

```cpp
client->async_call("Service.Method", body);
client->async_call_ct<method_id("Service.Method")>(body);
```

* The string-based overload hashes the name at runtime (FNV-1a).
* The `_ct` overload uses a compile-time hash.

---

# Reader Loop

`RpcClient::reader_loop()` runs while `running_` is `true`:

1. Captures `stream_`.
   If null → exit.

2. Reads header (**28 bytes**) via `read_exact`.
   EOF or error → exit.

3. Parses header.
   Invalid magic/version → exit.

4. Reads payload if `hdr.length > 0`.

5. If `FLAG_ENCRYPTED` is present:

    * Treat payload as `IV[12] + CT + TAG[16]`.
    * Decrypt with AES-256-GCM using the per-connection key from TLS exporter.
    * On decrypt failure:

        * treat as protocol/crypto error
        * terminate loop (connection closed)
        * wake all pending calls with `"Connection closed"`.

   After successful decrypt, the loop works with **plaintext** payload.

6. Dispatches by `FrameType`:

---

## Response Frames

* Lookup `PendingCall` by `stream_id`.

* If not found → ignore (stale / unexpected).

* If `FLAG_ERROR`:

    * decode error payload (after decrypt if `FLAG_ENCRYPTED`)
    * set:

      ```cpp
      call->error = true;
      call->error_code = parsed_code;
      call->error_message = parsed_message;
      ```

* Else:

    * copy plaintext payload into `call->response`

* Trigger `call->event->set()`.

---

## Ping Frames

If client receives `Ping` from server:

* Builds a matching `Pong` frame.
* Sends under `write_mutex_`.
* No payload; flags mirror end-of-stream and transport bits.

---

## Pong Frames

* Lookup waiter in `ping_waiters_` by `stream_id`.
* If found → trigger event.
* Used by `async_ping()`.

---

## Unknown / server-only frames

* Logged
* Ignored

---

# Loop Termination

When `reader_loop()` exits:

1. `running_ = false`.

2. For all pending RPCs:

    * mark error `"Connection closed"`.
    * trigger each `PendingCall::event`.

3. For all ping waiters:

    * trigger events (Pong will never come).

4. Under `connect_mutex_`, reset `stream_ = nullptr`.

All waiting coroutines are guaranteed to be released.

---

# Ping / Pong

`async_ping()` is a built-in liveness probe:

1. `ensure_connected()`.

2. Allocate `stream_id`.

3. Insert `AsyncEvent` into `ping_waiters_`.

4. Send `Ping` frame:

    * `type = Ping`
    * `flags = FLAG_END_STREAM` (+ optional `FLAG_TLS` / `FLAG_MTLS`)
    * no payload, `FLAG_ENCRYPTED` is **not** used.

5. Wait for `Pong` via event.

6. If waiter is still present → ping success; otherwise considered failed.

Use cases:

* keep-alive
* warm-up
* readiness checks / CLI pre-flight

---

# Closing the Client

`RpcClient::close()`:

* sets `running_ = false`
* swaps out `stream_`
* calls `stream->shutdown()`
* `reader_loop()` exits and does normal cleanup

This is a graceful shutdown path.

---

# TLS and mTLS Support

TLS is activated by selecting `TlsRpcStreamFactory`:

```cpp
urpc::RpcClientConfig cfg;
cfg.host = "server";
cfg.port = 45900;
// cfg.stream_factory will be set to a TLS-based factory
```

`TlsClientConfig` controls:

* `enabled` / `verify_peer`
* CA bundle
* client certificate/key (for mTLS)
* SNI/server_name
* optional app-level AES (key derived via TLS exporter)

mTLS requires:

* `verify_peer = true`
* client certificate + key
* matching server-side mTLS setup

From `RpcClient`’s perspective, TLS and mTLS are just different
`IRpcStream` implementations; the high-level behaviour of `async_call`,
`reader_loop`, `async_ping`, etc. does not change.

---

# CLI Tool

The repository includes a simple CLI client (`urpc_cli`) using the same core API.

It supports:

* plain TCP
* TLS / mTLS
* app-level AES when TLS is enabled (payload encryption via TLS exporter)

Examples:

### TCP

```bash
urpc_cli --host 127.0.0.1 --port 45900 \
         --method Example.Echo \
         --data "hello"
```

### TLS (server-auth only, payload encrypted with AES over TLS)

```bash
urpc_cli --tls --tls-ca ca.crt \
         --tls-server-name localhost \
         --host 127.0.0.1 --port 45900 \
         --method Example.Echo \
         --data "hello over tls+aes"
```

### mTLS (mutual TLS, payload encrypted with AES over TLS)

```bash
urpc_cli --tls \
         --tls-ca ca.crt \
         --tls-cert client.crt \
         --tls-key client.key \
         --tls-server-name localhost \
         --host 127.0.0.1 --port 45900 \
         --method Example.Echo \
         --data "hello over mtls+aes"
```

Behaviour:

* Header (28 bytes) is always visible at uRPC level.
* When TLS is enabled and app-level AES is on, the CLI encrypts only the payload
  (request and response bodies) with AES-256-GCM using a key derived from
  `SSL_export_keying_material`, and uses `FLAG_ENCRYPTED` in the frame flags.
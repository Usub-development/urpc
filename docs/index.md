# uRPC Protocol

uRPC is a compact binary RPC protocol running over a generic asynchronous byte
stream. The transport layer is abstracted through `IRpcStream`, allowing multiple
backends:

- TCP (`TcpRpcStream`)
- TLS (`TlsRpcStream`)
- mTLS (mutual TLS)
- Any custom transport implementing the interface

The protocol is designed for:

* Low overhead (**fixed 28-byte header**, big-endian encoding)
* Full multiplexing over a single connection
* Efficient coroutine-based processing (via **uvent**)
* Extensibility through frame types, flags, and open versioning
* Optional per-connection **AES-256-GCM payload encryption** on top of TLS

---

# Important Encryption Note

uRPC framing never encrypts the header.

* The **28-byte header is always plaintext** at the protocol level.
* **TLS/mTLS** encrypt the underlying TCP stream, but the header is still parsed
  in its normal plaintext form by uRPC.
* **App-level AES (FLAG_ENCRYPTED)** encrypts **only the payload**, never the header.
* AES keys are derived from the TLS exporter when app-level encryption is enabled.

Payload flow:

```

plaintext body
↓ (AES-256-GCM if enabled)
IV[12] + ciphertext[...] + TAG[16]

```

Header is unaffected.

---

# High-Level Concepts

### **Connection**

A long-lived bidirectional byte stream (TCP, TLS, mTLS, or user-defined).

### **Stream**

A logical RPC exchange within a connection, identified by a non-zero `stream_id`.

Multiple concurrent RPCs are multiplexed over a single transport instance.

### **Frame**

A single protocol message:

```

+-------------------+-----------------------------+
| Fixed Header      | Payload (binary, length N) |
+-------------------+-----------------------------+

```

Always starts with a **28-byte header**, followed by an optional payload  
(raw or AES-encrypted depending on flags and transport).

### **Method**

A remotely callable function identified by a 64-bit numeric ID:

```

method_id = fnv1a64("Service.Method")

```

The server uses this ID to look up the registered handler.

---

# Basic RPC Lifecycle

1. Client creates a stream:
    - `TcpRpcStream` (plain TCP)
    - `TlsRpcStream` (TLS)
    - TLS + client cert (mTLS)
    - Custom `IRpcStream`

   Transport selection is done through `RpcClientConfig.stream_factory`.

   If TLS is used and app-level AES is enabled, an AES key is derived immediately
   after the TLS handshake.

2. Client sends a **Request** frame:
    - `type = Request`
    - Non-zero `stream_id`
    - `method_id`
    - Payload = raw or AES-encrypted request body

3. Server `RpcConnection` loop:
    - Reads header (always plaintext)
    - Reads payload
    - If `FLAG_ENCRYPTED` → decrypt AES payload
    - Looks up handler in `RpcMethodRegistry`
    - Invokes coroutine handler:
      ```cpp
      Awaitable<std::vector<uint8_t>>(RpcContext&, std::span<const uint8_t>)
      ```

4. Handler returns the response body.

5. Server sends a **Response** frame:
    - Same `stream_id`
    - Same `method_id`
    - Payload = raw or AES-encrypted response bytes

6. Client’s `RpcClient`:
    - Reads header
    - Reads payload (decrypts if needed)
    - Matches by `stream_id`
    - Wakes the awaiting coroutine

---

# Concurrency Model

* One TCP/TLS/mTLS connection can carry many RPCs concurrently.
* Client allocates unique stream IDs atomically.
* Writes are serialized using an async mutex to preserve framing.
* Server creates one `RpcConnection` per accepted socket.
* Each connection has its own read loop coroutine.
* All handlers execute concurrently.

This avoids protocol-level head-of-line blocking.

---

# Transport Layer (TCP/TLS/mTLS + AES)

Transport is fully pluggable. The client/server receive a factory:

```cpp
std::shared_ptr<IRpcStreamFactory> stream_factory;
```

Built-in factories:

| Factory               | Transport |
|-----------------------|-----------|
| `TcpRpcStreamFactory` | TCP       |
| `TlsRpcStreamFactory` | TLS/mTLS  |

`TlsRpcStreamFactory` supports:

* TLS server authentication (CA, hostname)
* mTLS (client certificate)
* TLS handshake performed inside the stream
* Optional **app-level AES**, derived from TLS exporter

AES only applies to **payload**; header always remains plaintext.

---

# CLI Tool

The repository includes `urpc_cli`, a minimal command-line client implementing:

* TCP
* TLS
* mTLS
* App-level AES when TLS is enabled

Examples:

### Plain TCP

```
urpc_cli --host 127.0.0.1 --port 45900 \
         --method Example.Echo \
         --data "hello"
```

### TLS (server-auth only, AES optional)

```
urpc_cli --tls --tls-ca ca.crt \
         --tls-server-name localhost \
         --host 127.0.0.1 --port 45900 \
         --method Example.Echo \
         --data "hello"
```

### mTLS (client certificate + AES optional)

```
urpc_cli --tls \
         --tls-ca ca.crt \
         --tls-cert client.crt \
         --tls-key client.key \
         --tls-server-name localhost \
         --host 127.0.0.1 --port 45900 \
         --method Example.Echo \
         --data "hello secure"
```

---

# Next Sections

* [wire-format.md](wire-format.md) — Header layout, payload rules, flags
* [transport-and-ids.md](transport-and-ids.md) — Method IDs, connection rules
* [client.md](client.md) — Stream allocation, AES decrypt, reader loop
* [client-pool.md](client-pool.md) — Client pool 
* [server.md](server.md) — Accept loop, AES decrypt, handler invocation, shutdown
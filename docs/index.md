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

This documentation covers:

* Wire format
* Frame types and semantics
* Method identification
* Client and server behaviour
* Ping/pong and cancellation
* TLS and mTLS usage

---

# High-Level Concepts

### **Connection**

A long-lived bidirectional byte stream (TCP, TLS, mTLS, or user-defined).

### **Stream**

A logical RPC exchange within a connection, identified by a `stream_id`:

- `stream_id != 0`
- Multiple concurrent RPCs are multiplexed on the same underlying transport.

### **Frame**

A single protocol message:

```

+-------------------+-----------------------------+
| Fixed Header      | Payload (binary, length N) |
+-------------------+-----------------------------+

```

Always starts with a **28-byte header**, followed by an optional payload.

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
    - `TlsRpcStream` + client cert (mTLS)
    - Custom `IRpcStream`

   Transport selection is done through `RpcClientConfig.stream_factory`.

2. Client sends a **Request** frame:
    - `type = Request`
    - Non-zero `stream_id`
    - `method_id = fnv1a64(name)`
    - Payload = binary request body

3. Server’s `RpcConnection` loop:
    - Reads and parses the frame
    - Finds handler in `RpcMethodRegistry`
    - Invokes handler coroutine:
      ```cpp
      Awaitable<std::vector<uint8_t>>(RpcContext&, std::span<const uint8_t>)
      ```

4. Handler returns binary response.

5. Server sends a **Response** frame:
    - Same `stream_id`
    - Same `method_id`
    - Payload = response bytes

6. Client’s `RpcClient` matches the response by `stream_id` and resumes the awaiting coroutine.

---

# Concurrency Model

* A single TCP/TLS connection carries many parallel RPCs.
* Client allocates stream IDs using an atomic counter.
* All writes are serialized with an async mutex (`write_mutex_`) to preserve framing.
* Server creates one `RpcConnection` per accepted socket.
* Each connection has its own read loop coroutine.
* All handlers run concurrently.

This design eliminates head-of-line blocking inside the protocol layer.

---

# Transport Layer (TCP/TLS/mTLS)

Transport is pluggable. The client/server receive a factory:

```cpp
std::shared_ptr<IRpcStreamFactory> stream_factory;
```

Built-in factories:

| Factory               | Transport |
|-----------------------|-----------|
| `TcpRpcStreamFactory` | TCP       |
| `TlsRpcStreamFactory` | TLS/mTLS  |

`TlsRpcStreamFactory` configures:

* CA verification
* Server certificate verification (SNI)
* Optional client certificate (mTLS)
* TLS handshake done inside stream

Server-side factory enables TLS or mTLS by providing certificates and CA.

---

# CLI Tool

The repository includes `urpc_cli`, a minimal command-line client using the same protocol.

Examples:

### Plain TCP

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
         --method Example.Echo \
         --data "hello secure"
```

---

# Next Sections

* [wire-format.md](wire-format.md) — Header layout, frame types, flags
* [transport-and-ids.md](transport-and-ids.md) — Method IDs, connection rules
* [client.md](client.md) — Reader loop, stream allocation, ping/pong
* [server.md](server.md) — Accept loop, handler invocation, shutdown
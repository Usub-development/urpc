# uRPC Protocol

uRPC is a compact binary RPC protocol built on top of a generic async byte stream. It is designed for:

* Low overhead (fixed 28-byte header, no text encoding).
* Easy integration with C++ coroutines (via **uvent**).
* Extensibility (frame types, flags, reserved fields, versioning).

This document describes the protocol as implemented in the `urpc` library:

* Wire format (frames, headers, byte order).
* Message types, flags, and semantics.
* Client and server behaviour.
* Error, ping/pong, and cancellation semantics.
* Mapping between method names and numeric IDs.

## High-level concepts

* **Connection** – a long-lived bidirectional byte stream (typically a TCP connection).
* **Stream** – a logical RPC exchange inside a connection, identified by `stream_id` (32-bit).
* **Frame** – a single protocol message; always starts with a fixed header, followed by an optional payload.
* **Method** – a remotely callable function identified by a 64-bit `method_id` (an FNV-1a hash of the method name by default).

### Basic RPC lifecycle

1. Client connects to the server using a `TcpRpcStream` (or any `IRpcStream` implementation).
2. Client sends a **Request** frame:

    * `type = Request`
    * New `stream_id != 0`
    * `method_id` set to the target method
    * `payload` contains the serialized request body.
3. Server’s `RpcConnection` loop:

    * Reads the header and payload.
    * Looks up the handler in `RpcMethodRegistry` by `method_id`.
    * Invokes the handler coroutine with an `RpcContext` and the request payload.
4. Handler returns a `std::vector<uint8_t>` response body.
5. Server sends a **Response** frame:

    * `type = Response`
    * Same `stream_id` and `method_id`.
    * `payload` contains the serialized response body.
6. Client’s `RpcClient` reader loop:

    * Matches the Response by `stream_id`.
    * Fulfils the pending call’s `AsyncEvent`.
    * Returns the response bytes to the caller.

### Concurrency model

* A single connection may carry multiple concurrent RPCs, each with its own `stream_id`.
* Client uses `RpcClient::next_stream_id_` (atomic) to allocate stream IDs.
* Writes are serialized with an async mutex (`write_mutex_`) on both client and server.
* Server uses `RpcConnection` per accepted connection; each connection runs its own read loop coroutine.
# uRPC Wire Protocol

This document describes the wire format used by the uRPC system, as defined in `Wire.h` and `Codec.h`. It covers
framing, message types, flags, handshake, streams, limits, and payload encoding for both unary and streaming RPCs.

## Goals

* Small, fixed header with a contiguous frame structure.
* Symmetric communication for client and server.
* Transport-agnostic (supports RAW, TLS, mTLS).
* Compatible with both unary and streaming RPCs.

---

## Frame Structure

Each message is a single frame with the following structure:

```

+-----------------+-------------------------------+
| 4B len (LE)     | Total payload size (after len)|
+-----------------+-------------------------------+
| 1B ver | Protocol version (1)          |
| 1B type | Message type (MsgType)        |
| 1B flags | Transport + per-frame flags |
| 1B rsv | Reserved (0)                  |
| 4B stream (LE)  | Stream ID |
| 8B method (LE)  | Method ID (fnv1a64(name))     |
| 4B meta_len(LE) | Metadata length |
| 4B body_len(LE) | Body length |
+-----------------+-------------------------------+
| meta bytes... | Length = meta_len |
+-----------------+-------------------------------+
| body bytes... | Length = body_len |
+-----------------+-------------------------------+

```

### Constants:

* `HDR_NO_LEN = 24`, `HDR_SIZE = 28`.
* `len = HDR_NO_LEN + meta_len + body_len`.
* Maximum payload size: `meta_len + body_len ≤ 16 MiB` (`MAX_FRAME_NO_LEN`).

### Endianness:

* All multi-byte integers are in **little-endian** format.

`rsv` (reserved) must always be `0` when sent and is ignored when received.

---

## Message Types (`MsgType`)

The following message types are defined:

* `REQUEST = 0`
* `RESPONSE = 1`
* `ERROR = 2`
* `CANCEL = 3`
* `PING = 4`
* `PONG = 5`
* `GOAWAY = 6`

### Rules:

* **Unary RPC**: The client sends a `REQUEST`, and the server replies with a `RESPONSE` or `ERROR`.
* **Streaming RPC**: Multiple `REQUEST`/`RESPONSE` frames can flow on the same `stream` until one side sets
  `F_STREAM_LAST`.
* **Control frames**:
    * `PING`/`PONG` are for liveness checking, with no other semantics.
    * `CANCEL` indicates to stop processing the stream.
    * `GOAWAY` signals the peer that no new streams will be accepted.

---

## Flags

### Transport flags (low 2 bits):

* `F_TP_RAW = 0b00`
* `F_TP_TLS = 0b01`
* `F_TP_MTLS = 0b10`
* `F_TP_CUSTOM = 0b11`

### Per-frame flags:

* `F_COMPRESSED = 1 << 2` — Indicates that the body is compressed (codec-defined).
* `F_CB_PRESENT = 1 << 3` — Indicates that callback metadata is present.
* `F_STREAM_LAST = 1 << 4` — Marks the last message for this stream.
* `F_FLOW_CREDIT = 1 << 5` — Indicates that the body carries a flow credit grant.

### Helper Functions:

* `transport_matches(is_tls, is_mtls, flags)` — Checks whether the transport bits match the actual transport used.

---

## Streams and Methods

* **Stream ID**: A 32-bit identifier. `0` is reserved for the settings handshake.
* **Method ID**: A 64-bit identifier derived from the RPC method name using **FNV-1a** hashing:
  `fnv1a64_fast("ServiceOrMethodName")`.

---

## Settings Handshake (Stream 0)

The first frame from the client **must** be a `REQUEST` on `stream=0`, `method=0`:

* **type = REQUEST**
* **stream = SETTINGS_STREAM (0)**
* **method = SETTINGS_METHOD (0)**
* **flags**: Transport bits reflect the negotiated transport (`RAW`, `TLS`, or `mTLS`).
* **meta** and **body** are optional and application-defined (often empty).

The server replies with a `RESPONSE` on the same stream/method, echoing the transport bits. After that, normal RPC
communication begins.

The server may optionally dispatch the first `REQUEST` immediately (depending on implementation).

### Validation:

`validate_first_settings(pf, is_tls, is_mtls)` checks if the transport bits in the header match the actual transport
used.

---

## Unary RPC

Client:

1. Choose a `stream = next()`.
2. Send a `REQUEST` with `method = fnv1a64_fast(name)`.
3. Encode the `meta` and `body` (see **Payload Encoding**).
4. Send the frame and wait for a response with the same `stream`:
    * On `RESPONSE`: Decode `meta` (optional) and `body` into the response type.
    * On `ERROR`: Decode the `body` as an `RpcError`.

Server:

* On `REQUEST`, decode the `body` into the request type, process it, and send a `RESPONSE` or `ERROR`.

---

## Streaming RPC (Base)

* Each side can send multiple frames on the same `stream`.
* The sender marks the last frame with `F_STREAM_LAST`.
* **Flow credit**: If `F_FLOW_CREDIT` is set, the body of the frame carries a varuint **credit** value that informs the
  peer how many additional frames it can send on the stream.

---

## Control Frames

* **PING**: A `PING` frame is sent by the client (typically `stream=0`), and the server replies with a `PONG` echoing
  the `stream` and `flags`.
* **CANCEL**: A `CANCEL` frame is sent to the peer to stop processing a particular stream and halt sending further
  `RESPONSE`s. The peer may send an `ERROR CANCELLED` frame if needed.
* **GOAWAY**: The `GOAWAY` frame signals that the peer will not accept new streams. Existing streams may continue until
  completion.

---

## Errors and Status Codes

The `ERROR` frame carries a status code and an optional message in the **body**:

```cpp
enum class StatusCode : uint16_t {
    OK = 0, CANCELLED = 1, UNKNOWN = 2, INVALID_ARGUMENT = 3, DEADLINE_EXCEEDED = 4,
    NOT_FOUND = 5, ALREADY_EXISTS = 6, PERMISSION_DENIED = 7, RESOURCE_EXHAUSTED = 8,
    FAILED_PRECONDITION = 9, ABORTED = 10, OUT_OF_RANGE = 11, UNIMPLEMENTED = 12,
    INTERNAL = 13, UNAVAILABLE = 14, DATA_LOSS = 15, UNAUTHENTICATED = 16
};

struct RpcError {
    StatusCode code;
    std::string message;
};
````

### Examples:

* Bad request body → `INVALID_ARGUMENT`
* Handler threw an exception → `INTERNAL`
* Unknown method → `NOT_FOUND`
* Deadline exceeded → `DEADLINE_EXCEEDED`
* Client cancelled the request → `CANCELLED`

---

## Limits

* **Max Payload Size**: `meta_len + body_len ≤ 16 MiB`.
* **Versioning**: `ver` must be `1` (receivers should drop frames with unknown versions).
* **Reserved field**: `rsv` should be `0` and must be ignored by receivers.

---

## Payload Encoding (`Codec.h`)

### Primitives

* **Unsigned integers**: **varuint** (LEB128-style encoding).
* **Signed integers**: **zig-zag** encoding + varuint.
* **bool**: 1 byte (`0/1`).
* **float**/`double`: Raw IEEE-754 bytes (4/8).
* **std::string**: `varuint len` + bytes.
* **std::array<T,N>**: Encodes `N` inlined elements.
* **std::vector<T>**: `varuint count` + elements.
* **std::optional<T>**: 1 byte (`0/1`) followed by `T` if present.
* **enum**: Encodes the underlying integer.
* **std::tuple<...>**: Encodes each element in the tuple in order.

### Structs

Any struct or aggregate type supported by `ureflect` is encoded as a sequence of fields in declaration order.

### Varints

* **Max shift**: `MAX_VARINT_SHIFT = 63`.

---

## Method IDs

Method IDs are calculated using **FNV-1a** hashing:

```cpp
method = fnv1a64_fast(name) // where `name` is the UTF-8 string representation of the method.
```

---

## Transports

The transport protocol is external to the wire format, and the header's low 2 bits represent the transport in use:

* **RAW**: Plain TCP.
* **TLS**: Encrypted connection using TLS.
* **MTLS**: Mutual TLS (client and server both authenticate).

Receivers should verify the transport type by checking the flags against the actual transport being used.

---

## Handshake Walkthrough

**Client:**

1. Connect (RAW/TLS/mTLS as configured).
2. Send the `SETTINGS` frame: `REQUEST`, `stream=0`, `method=0`, with the transport bits set.
3. Wait for the server's `RESPONSE` on `stream=0`.
4. Begin sending RPCs.

**Server:**

1. Receive the first frame.
2. If it's a `SETTINGS` frame on `stream=0`, respond with a `RESPONSE` and proceed.
3. If not, optionally dispatch the first `REQUEST` immediately (the library does this).

---

## Flow Credit

If the `F_FLOW_CREDIT` flag is set, the body of the frame carries a varuint `credit`. This informs the peer that it may
send up to that many additional frames on the stream.

---

## Compression

If the `F_COMPRESSED` flag is set, the body is compressed. The compression algorithm is determined by the codec used and
is negotiated in the metadata (application-defined).

---

## Versioning and Extensibility

* The **version (`ver`)** field must be `1` to maintain compatibility.
* The **reserved (`rsv`)** field should remain `0` and is ignored by receivers.
* Any new flags must not reuse existing bits; unknown flags should be ignored by receivers.

---

## Compliance Checklist

* [ ] First client frame must be `SETTINGS` on `stream=0`.
* [ ] Transport bits must match the actual transport used.
* [ ] Payload sizes must not exceed 16 MiB per frame.
* [ ] `ver == 1`, `rsv == 0`.
* [ ] Routing must strictly follow `stream` and `method`.
* [ ] `ERROR` frames must contain an `RpcError` in the body.
* [ ] Stream termination must respect `F_STREAM_LAST`.
* [ ] The server must respond with `PONG` to `PING`.

---

## Example

### Client Unary Request (`Echo`)

```
type=REQUEST
flags=RAW
stream=1
method=fnv1a64("Echo")
meta = <empty>
body = encode(EchoReq{ text="hi" })
```

### Server Response

```
type=RESPONSE
flags=RAW
stream=1
method=fnv1a64("Echo")
meta = <empty>
body = encode(EchoResp{ text="echo: hi" })
```

### Error Example (Unknown Method)

```
type=ERROR
flags=RAW
stream=1
method=<same method id>
meta = <empty>
body = encode(RpcError{ code=StatusCode::NOT_FOUND, message="unknown method" })
```

---

## Control Frames

* **PING/PONG**:

    * The client sends `PING` (usually `stream=0`).
    * The server replies with `PONG`, echoing `stream` and `flags`.

* **CANCEL**:

    * The client sends `CANCEL` for a specific `stream`.
    * The server should stop processing that stream and not send further `RESPONSE`s. An `ERROR CANCELLED` may be sent
      if needed.

* **GOAWAY**:

    * A `GOAWAY` frame signals that no new streams will be accepted. Existing streams may continue until completion.
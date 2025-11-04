# uRPC Wire Protocol

This describes the on-the-wire format used by uRPC (`Wire.h` + `Codec.h`). It covers framing, message types, flags,
handshake, streams, limits, and payload encoding.

## Goals

* Small, fixed header; contiguous frame.
* Symmetric client/server messaging.
* Transport-agnostic (RAW/TLS/mTLS).
* Works for unary and streaming RPC.

---

## Frame

Every message is a single frame:

```
+-----------------+-------------------------------+
| 4B len (LE)     | Total payload bytes after len |
+-----------------+-------------------------------+
| 1B ver          | Protocol version (1)          |
| 1B type         | MsgType                       |
| 1B flags        | Transport + per-frame flags   |
| 1B rsv          | Reserved (0)                  |
| 4B stream (LE)  | Stream ID                     |
| 8B method (LE)  | Method ID (fnv1a64(name))     |
| 4B meta_len(LE) | Metadata length               |
| 4B body_len(LE) | Body length                   |
+-----------------+-------------------------------+
| meta bytes...   | Length = meta_len             |
+-----------------+-------------------------------+
| body bytes...   | Length = body_len             |
+-----------------+-------------------------------+
```

Constants:

* `HDR_NO_LEN = 24`, `HDR_SIZE = 28`.
* `len = HDR_NO_LEN + meta_len + body_len`.
* Hard cap: `meta_len + body_len ≤ 16 MiB` (`MAX_FRAME_NO_LEN`).

Endianness: all multi-byte integers are **little-endian**.

`rsv`: reserved, must be `0` on send, ignored on receive.

---

## Message Types (`MsgType`)

* `REQUEST = 0`
* `RESPONSE = 1`
* `ERROR = 2`
* `CANCEL = 3`
* `PING = 4`
* `PONG = 5`
* `GOAWAY = 6`

Rules:

* Unary: client sends `REQUEST`, server replies with `RESPONSE` or `ERROR`.
* Streaming: multiple `REQUEST`/`RESPONSE` may flow on the same `stream` until one side sets `F_STREAM_LAST`.
* `PING`/`PONG` are control frames (no semantics beyond liveness).
* `CANCEL` asks the peer to stop work for a `stream`.
* `GOAWAY` indicates the peer won’t accept new streams (existing may continue).

---

## Flags

Transport bits (low 2 bits):

* `F_TP_RAW = 0b00`
* `F_TP_TLS = 0b01`
* `F_TP_MTLS = 0b10`
* `F_TP_CUSTOM = 0b11`

Per-frame flags:

* `F_COMPRESSED = 1 << 2` — body is compressed (codec-defined; currently informational).
* `F_CB_PRESENT = 1 << 3` — “callback/metadata present” hint (used by settings).
* `F_STREAM_LAST = 1 << 4` — last message on this stream from the sender.
* `F_FLOW_CREDIT = 1 << 5` — body carries flow-credit grant (see below).

Helper:

* `transport_matches(is_tls, is_mtls, flags)` checks transport bits against the actual connection.

---

## Streams and Methods

* `stream` is a 32-bit ID. `0` is **reserved** for the settings handshake.
* Clients pick stream IDs for new RPCs.
* `method` is the 64-bit **FNV-1a** hash of the RPC name: `fnv1a64_fast("ServiceOrMethodName")`.

---

## Settings Handshake (stream 0)

First frame from the client **must** be a `REQUEST` on `stream=0`, `method=0`:

* `type = REQUEST`
* `stream = SETTINGS_STREAM (0)`
* `method = SETTINGS_METHOD (0)`
* `flags` transport bits reflect the negotiated transport (`RAW/TLS/mTLS`).
* `meta`/`body` are optional and application-defined (often empty).

Server replies with `RESPONSE` on the same stream/method, mirroring transport bits. After that, normal dispatch begins.
If the first frame is already a regular `REQUEST`, the server may dispatch it immediately (library does this) but the
settings path is the standard.

`validate_first_settings(pf, is_tls, is_mtls)` checks that `flags` transport bits match the real transport.

---

## Unary RPC

Client:

1. Pick `stream = next()`.
2. Build `REQUEST` with `method = fnv1a64_fast(name)`.
3. Encode `meta` and `body` (see **Payload Encoding**).
4. Send frame and wait for a frame with the **same `stream`**:

    * On `RESPONSE`: decode `meta` (optional) and `body` into the response type.
    * On `ERROR`: decode `body` as `RpcError`.

Server:

* On `REQUEST`, decode `body` into the request type, run handler, reply with `RESPONSE` (or `ERROR`).

---

## Streaming RPC (base)

* Each side may send multiple frames on the same `stream`.
* Sender sets `F_STREAM_LAST` on its final frame.
* Basic credit signaling is reserved via `F_FLOW_CREDIT` (when set, the `body` carries a varuint **credit** value).
  Current library exposes helpers to send a credit grant, but flow control is minimal by default.

---

## Control Frames

* **PING**: `type=PING`, any `stream` (typically `0`), optional echo of `flags`. Peer replies with `PONG` carrying the
  same `stream` and `flags`.
* **CANCEL**: `type=CANCEL`, `stream=<target>`, body/meta empty. Peer should attempt to stop work on that stream and
  stop sending further `RESPONSE`s (may send an `ERROR CANCELLED` if a terminal message is needed).
* **GOAWAY**: `type=GOAWAY`. No fixed semantics yet other than “no new streams”.

---

## Errors and Status Codes

`ERROR` frames carry a canonical status and message in **body**:

```cpp
enum class StatusCode : uint32_t {
  OK=0, CANCELLED=1, UNKNOWN=2, INVALID_ARGUMENT=3, DEADLINE_EXCEEDED=4,
  NOT_FOUND=5, ALREADY_EXISTS=6, PERMISSION_DENIED=7, RESOURCE_EXHAUSTED=8,
  FAILED_PRECONDITION=9, ABORTED=10, OUT_OF_RANGE=11, UNIMPLEMENTED=12,
  INTERNAL=13, UNAVAILABLE=14, DATA_LOSS=15, UNAUTHENTICATED=16
};

struct RpcError { StatusCode code; std::string message; };
```

Encoding uses the same rules as any struct in **Payload Encoding** (enum underlying value then string).

Examples:

* Bad request body → `INVALID_ARGUMENT`
* Handler threw → `INTERNAL`
* Unknown method → `NOT_FOUND`
* Deadline exceeded → `DEADLINE_EXCEEDED`
* Cancelled by client → `CANCELLED`

---

## Limits

* Max `meta_len + body_len` per frame: **16 MiB**.
* `ver` must be `1` (receivers should drop frames with unknown version).

---

## Payload Encoding (`Codec.h`)

Binary codec for `meta` and `body`.

### Primitives

* Unsigned integers: **varuint** (LEB128-style).
* Signed integers: **zig-zag** + varuint.
* `bool`: 1 byte `0/1`.
* `float`/`double`: raw IEEE-754 bytes (4/8).
* `std::string`: `varuint len` + bytes.
* `std::array<T,N>`: N inlined values.
* `std::vector<T>`: `varuint count` + elements.
* `std::optional<T>`: 1 byte `0/1` then `T` if present.
* `enum`: encode underlying integer (via varuint).
* `std::tuple<...>`: elements in order.

### Structs

Any aggregate supported by `ureflect` is encoded as a **field sequence** in declaration order (no field tags). Both
sides must share the same layout contract for a given type.

### Varints

* Max shift guard: `MAX_VARINT_SHIFT = 63`.

---

## Method IDs

`method = fnv1a64_fast(name)` where `name` is a UTF-8 string. Same hashing is used consistently on both sides.

---

## Transports

Transport is external to the wire format; the header’s low 2 bits record the transport in use:

* `RAW`: plain TCP.
* `TLS`: TLS.
* `MTLS`: mutual TLS.

Receivers should verify `flags` against the actual transport (`transport_matches`).

---

## Handshake Walkthrough

Client:

1. Connect (RAW/TLS/mTLS as configured).
2. Send `SETTINGS` (`REQUEST`, `stream=0`, `method=0`, transport bits set).
3. Wait for `RESPONSE` on `stream=0`.
4. Start RPCs.

Server:

1. Receive first frame.
2. If it’s `SETTINGS` on stream 0: respond `RESPONSE`, then proceed.
3. Else: optionally dispatch the first `REQUEST` immediately (library does).

---

## Flow Credit

If `F_FLOW_CREDIT` is set, the frame `body` carries a single varuint `credit`. It informs the peer it may send up to
that many additional payload frames on this stream. The current implementation exposes helpers to grant credit; strict
enforcement is left to the application.

---

## Compression

`F_COMPRESSED` is a hint that `body` is compressed. The choice of algorithm and its negotiation live in `meta` (
application-defined). The core library does not apply compression automatically.

---

## Versioning and Extensibility

* `ver` gates incompatible changes.
* `rsv` reserved for future use; keep `0`.
* New flags must not reuse existing bits; receivers ignore unknown per-frame flags except transport bits.

---

## Parsing Rules (Receiver)

1. Ensure buffer has ≥ `HDR_SIZE` bytes.
2. Read `len` (LE32). If `len < HDR_NO_LEN` or `len > HDR_NO_LEN + MAX_FRAME_NO_LEN`, drop.
3. Ensure buffer has `4 + len` bytes.
4. Decode header fields and check `len == HDR_NO_LEN + meta_len + body_len`.
5. Slice `meta`, `body` accordingly.
6. Route by `type`, `stream`, `method`.

---

## Example

**Client unary request** (`Echo`):

```
type=REQUEST
flags=RAW
stream=1
method=fnv1a64("Echo")
meta = <empty>
body = encode(EchoReq{ text="hi" })
```

**Server response**:

```
type=RESPONSE
flags=RAW
stream=1
method=fnv1a64("Echo")
meta = <empty>
body = encode(EchoResp{ text="echo: hi" })
```

**Error example** (unknown method):

```
type=ERROR
flags=RAW
stream=1
method=<same method id>
meta = <empty>
body = encode(RpcError{ code=StatusCode::NOT_FOUND, message="unknown method" })
```

---

## Control Sequences

* **Ping/Pong**:

    * Client sends `PING` (usually `stream=0`).
    * Server replies `PONG` echoing `stream` and `flags`.

* **Cancel**:

    * Client sends `CANCEL` with target `stream`.
    * Server should stop the handler for that stream and avoid sending further `RESPONSE`. If a terminal message is
      needed, send `ERROR CANCELLED`.

* **GoAway**:

    * Peer may send `GOAWAY` to refuse new streams. Existing streams may finish.

---

## Compliance Checklist

* [ ] First client frame is `SETTINGS` on `stream=0`.
* [ ] Transport bits match actual transport.
* [ ] Never exceed 16 MiB payload per frame.
* [ ] `ver==1`, `rsv==0`.
* [ ] Route strictly by `stream`.
* [ ] For `ERROR`, body encodes `RpcError`.
* [ ] Respect `F_STREAM_LAST` for stream termination.
* [ ] Reply `PONG` to `PING`.

---

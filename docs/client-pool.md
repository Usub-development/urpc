# **RPC Client Pool**

`RpcClientPool` is a lightweight, lock-free, dynamic pool of `RpcClient` instances.
It distributes outgoing RPC calls across multiple independent connections while relying on the internal multiplexing
capabilities of each `RpcClient`.

Unlike traditional connection pools, this pool:

* **expands on demand**, up to `max_clients`,
* returns **references**, never pointers,
* performs **zero locking** (only atomics),
* provides **strict O(1)** client selection.

---

## **Key Behavior**

### ✔ Dynamic creation

Clients are created lazily when needed.
If `max_clients` is not reached, `try_acquire()` constructs a new `RpcClient`.

### ✔ Unlimited mode

If you set:

```cpp
.max_clients = std::numeric_limits<std::size_t>::max()
```

the pool effectively becomes unbounded — connections grow as needed.

### ✔ Multiplexed client reuse

If all clients are already created, the pool picks one via **round-robin**:

* No locks.
* No contention.
* Perfect distribution across existing connections.

Each `RpcClient` internally serializes writes and multiplexes responses, so using the same client from multiple
coroutines is safe.

---

## **Usage Example**

```cpp
urpc::RpcClientPoolConfig cfg{
    .host = "127.0.0.1",
    .port = 45900,
    .stream_factory = nullptr,
    .socket_timeout_ms = -1,
    .ping_interval_ms = 0,
    .max_clients = 4
};

urpc::RpcClientPool pool{cfg};

auto lease = pool.try_acquire();
urpc::RpcClient& client = lease.client;

auto resp = co_await client.async_call("Example.Echo", body);
```

The returned lease contains:

```cpp
RpcClient& client;
size_t index;
```

No pointers. No ownership complexities. No risk of dangling references.

---

## **Internals**

### **1. On-demand creation**

```cpp
if (size < max_clients)
    create new RpcClient
```

Creation is guarded only by an atomic `size_` CAS.
`LockFreeVector::emplace_back` provides safe concurrent growth.

### **2. Round-robin**

Once the pool reaches configured capacity:

```cpp
idx = rr_.fetch_add(1) % size_;
return clients_.at(idx);
```

Fast, predictable, starvation-free.

### **3. Client independence**

Each `RpcClient` maintains:

* its own TCP/TLS socket,
* its own reader coroutine,
* its own queue of pending calls,
* its own write-ordering mutex (internal).

Therefore:

* using the same client in 1000 coroutines is safe,
* but spreading load across several clients reduces write mutex contention and improves throughput.

---

## **Why No Mutexes or Semaphores in the Pool?**

* `ConcurrentVector` already ensures safe concurrent growth.
* `RpcClient` itself is concurrency-safe; it multiplexes calls.
* Round-robin is just an atomic counter.

Anything else would be extra overhead.

---

## **RpcClientLease**

The pool never returns raw pointers.
Instead, a lease holds:

```cpp
RpcClient& client;
size_t index;
```

This avoids:

* ownership confusion,
* accidental deletion,
* cache-unfriendly pointer chasing.

---

## **Example: 16 concurrent workers**

```cpp
static task::Awaitable<void> worker(
    urpc::RpcClientPool& pool, size_t id)
{
    auto lease = pool.try_acquire();
    auto& client = lease.client;

    std::string msg = "hello from worker " + std::to_string(id);
    std::span<const uint8_t> body{
        reinterpret_cast<const uint8_t*>(msg.data()),
        msg.size()
    };

    auto resp = co_await client.async_call("Example.Echo", body);
    ulog::info("worker {} resp={}", id, resp.size());
}
```

This pattern supports thousands of concurrent RPC calls without bottlenecks.

---

## **When to Use the Pool**

### Recommended:

* bursty or high parallel RPC workloads
* heavy writes
* situations where a single TCP connection becomes a bottleneck
* services that require scaling outbound throughput

### Not needed:

* low-rate RPC usage
* strictly ordered global streams

---

## **Performance Notes**

* Creating connections lazily avoids unnecessary sockets.
* Lock-free round-robin is extremely cheap.
* Multiple `RpcClient` instances reduce per-client mutex contention.
* Client lifetime is stable (vector never shrinks).
* Cache locality is good: contiguous array of clients.

---

## **Best Practices**

### ✔ Good:

* Use `max_clients = SIZE_MAX` if you want elastic scaling.
* Keep pool as a global or static subsystem — never recreate per request.
* Use leases immediately; don’t store them long-term.

### ✘ Avoid:

* Adding external synchronization around pool calls.
* Returning smart pointers instead of references.
* Destroying pool while active requests exist.
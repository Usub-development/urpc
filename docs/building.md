# Building uRPC

This page describes how to build **urpc**, control logging, CLI, examples and tests.

## Requirements

- CMake **≥ 3.27**
- C++23 compiler (GCC / Clang / MSVC with C++23)
- OpenSSL (Crypto + SSL)
- Git (for `FetchContent` dependencies)

uRPC uses:

- [`uvent`](https://github.com/Usub-development/uvent)
- [`ureflect`](https://github.com/Usub-development/ureflect)
- [`ulog`](https://github.com/Usub-development/ulog) (when logging / CLI is enabled)
- GoogleTest (optional, for tests)

All of them are pulled via `FetchContent` by default.

---

## FetchContent (recommended for embedding)

If you want to **embed uRPC directly into your project** without installation, use CMake’s `FetchContent`.

### Minimal example

```cmake
include(FetchContent)

FetchContent_Declare(
        urpc
        GIT_REPOSITORY https://github.com/Usub-development/urpc.git
        GIT_TAG main
)

FetchContent_MakeAvailable(urpc)

add_executable(my_app main.cpp)

target_link_libraries(my_app
        PRIVATE
        usub::urpc
)
```

### With logging disabled

```cmake
FetchContent_Declare(
        urpc
        GIT_REPOSITORY https://github.com/Usub-development/urpc.git
        GIT_TAG main
)

set(URPC_LOGS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(urpc)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE urpc)
```

### With CLI disabled / examples disabled

```cmake
set(URPC_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(URPC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
        urpc
        GIT_REPOSITORY https://github.com/Usub-development/urpc.git
        GIT_TAG main
)
FetchContent_MakeAvailable(urpc)
```

### Notes

* All transitive dependencies (`uvent`, `ureflect`, `ulog` if enabled) are fetched automatically.
* No system installation is required.
* Works well for monorepos and statically linked backend binaries.

## Basic build

```bash
git clone https://github.com/Usub-development/urpc.git
cd urpc

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug

cmake --build build
```

Install:

```bash
cmake --install build --prefix /usr/local
```

---

## CMake options

| Option                | Default | Description                          |
|-----------------------|---------|--------------------------------------|
| `URPC_LOGS`           | `OFF`   | Enable internal logging via **ulog** |
| `URPC_BUILD_CLI`      | `ON`    | Build `urpc_cli` command-line tool   |
| `URPC_BUILD_EXAMPLES` | `ON`    | Build example servers/clients        |
| `URPC_BUILD_TESTS`    | `ON`    | Build tests (if `BUILD_TESTING=ON`)  |

### Example: minimal build (no logs, no CLI, no examples, no tests)

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DURPC_LOGS=OFF \
  -DURPC_BUILD_CLI=OFF \
  -DURPC_BUILD_EXAMPLES=OFF \
  -DURPC_BUILD_TESTS=OFF

cmake --build build
```

---

## Logging (`URPC_LOGS`)

```cmake
option(URPC_LOGS "Use URPC_LOGS" ON)

if (URPC_LOGS)
    # ulog is fetched and linked only if logs are enabled
    FetchContent_Declare(
            ulog
            GIT_REPOSITORY https://github.com/Usub-development/ulog.git
            GIT_TAG main
    )
    FetchContent_MakeAvailable(ulog)

    target_link_libraries(urpc PUBLIC usub::ulog)
endif ()

target_compile_definitions(urpc PUBLIC
        $<$<BOOL:${URPC_LOGS}>:URPC_LOGS>
)
```

* `URPC_LOGS=ON`:
    * `urpc` links against `ulog`
    * all `#if URPC_LOGS` log statements are compiled in
* `URPC_LOGS=OFF`:
    * no dependency on `ulog`
    * all logging branches compiled out

**Build with logs disabled:**

```bash
cmake -S . -B build \
  -DURPC_LOGS=OFF
cmake --build build
```

---

## CLI tool (`URPC_BUILD_CLI`)

When `URPC_BUILD_CLI=ON`:

* `ulog` is fetched (if not already)
* `urpc_cli` is built and installed to `${CMAKE_INSTALL_BINDIR}`

Relevant CMake:

```cmake
option(URPC_BUILD_CLI "Build urpc_cli command-line tool" ON)

if (URPC_BUILD_CLI)
    FetchContent_Declare(
            ulog
            GIT_REPOSITORY https://github.com/Usub-development/ulog.git
            GIT_TAG main
    )
    FetchContent_MakeAvailable(ulog)
    target_link_libraries(urpc PUBLIC usub::ulog)

    add_executable(urpc_cli bin/urpc_cli.cpp)
    target_link_libraries(urpc_cli PRIVATE urpc usub::ulog)
    target_compile_definitions(urpc_cli PRIVATE DEV_STAGE=${DEV_STAGE})

    install(TARGETS urpc_cli
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif ()
```

**Disable CLI:**

```bash
cmake -S . -B build \
  -DURPC_BUILD_CLI=OFF
cmake --build build
```

---

## Examples (`URPC_BUILD_EXAMPLES`)

When `URPC_BUILD_EXAMPLES=ON`, the following binaries are built:

* `urpc_example`
* `urpc_example_server_mtls`
* `urpc_example_client`
* `urpc_stress_client`
* `urpc_example_multi`
* `urpc_example_client_multi`
* `urpc_stress_client_multi`

Each example is linked with `urpc` and compiled with `DEV_STAGE=${DEV_STAGE}`.

**Disable examples:**

```bash
cmake -S . -B build \
  -DURPC_BUILD_EXAMPLES=OFF
cmake --build build
```

---

## Tests (`URPC_BUILD_TESTS`)

Tests are guarded by:

```cmake
option(URPC_BUILD_TESTS "Build urpc tests" ON)

if (BUILD_TESTING AND URPC_BUILD_TESTS)
    # Fetch / find GoogleTest etc...
endif ()
```

To enable tests:

```bash
cmake -S . -B build \
  -DBUILD_TESTING=ON \
  -DURPC_BUILD_TESTS=ON

cmake --build build
```

(Currently test targets are commented out in `CMakeLists.txt`; they can be re-enabled later.)

---

## DEV_STAGE define

Several executables are compiled with:

```cmake
target_compile_definitions(urpc_cli PRIVATE DEV_STAGE=${DEV_STAGE})
target_compile_definitions(urpc_example PRIVATE DEV_STAGE=${DEV_STAGE})
# ... same for other examples
```

If you want to control `DEV_STAGE`, pass it via CMake:

```bash
cmake -S . -B build \
  -DDEV_STAGE=dev
cmake --build build
```

(Your code can use `#if DEV_STAGE == ...` or `ULOG` formatting based on this define.)

---

## Using installed urpc

After `cmake --install`:

```cmake
find_package(urpc REQUIRED)

add_executable(my_app main.cpp)

target_link_libraries(my_app
        PRIVATE
        urpc::urpc
)
```

Include headers as:

```cpp
#include <urpc/client/RPCClient.h>
#include <urpc/server/RPCServer.h>
#include <urpc/datatypes/Frame.h>
```

That’s enough to:

* toggle logs,
* toggle CLI,
* toggle examples/tests
# Extensions

Extensions connect graphs to external systems or add value types and operations.
Each extension has its own build requirements.

| Extension | Use | Python | C++ |
| --- | --- | --- | --- |
| [Web](web.md) | HTTP services, requests, authentication, and binary WebSocket connections | HTTP | HTTP and WebSocket |
| [Frame](frame.md) | Nullable columns, keyed tables, row updates, and column expressions | — | Supported |

The web extension uses [services](../reference/services.md) to connect transport
implementations to graph code. Its examples use the realtime engine because network
events arrive in wall-clock time.

The frame extension adds scalar and time series types. Its examples use
`run_graph` with literal inputs, as in [changing inputs](../guide/graphs/updates.md).

## Build requirements

### Web

Build BG with the web option. Python also requires the Python option:

```sh
cmake -S . -B build/web -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DBG_BUILD_WEB=ON -DBG_BUILD_PYTHON=ON -DCMAKE_INSTALL_PREFIX=install
cmake --build build/web --config Release
cmake --install build/web --config Release
```

Configuration requires libcurl 7.85 or later and Boost 1.87 or later. CMake can
download the required Boost components when a suitable installation is unavailable.
The installation includes `bg_web`, `<bg/web/...>` headers, and the `bg::web` target
in the BG CMake package. Python must be able to import packages from the installation
prefix.

The [C++ example project](../guide/installation.md#c-example-project) uses this target
for web examples:

```cmake
target_link_libraries(example PRIVATE bg::web)
```

### Frame

The frame extension currently builds from its source directory and has a C++ API.
With BG installed, a consuming project adds the frame source and links its target:

```cmake
cmake_minimum_required(VERSION 3.31)
project(example LANGUAGES CXX)
find_package(bg CONFIG REQUIRED)
set(BUILD_TESTING OFF CACHE BOOL "Build tests")
add_subdirectory("${BG_SOURCE_DIR}/extensions/frame" frame)
add_executable(example main.cpp)
target_link_libraries(example PRIVATE bg::frame)
```

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/absolute/path/to/bg/install -DBG_SOURCE_DIR=/absolute/path/to/bg
cmake --build build --config Release
```

The native frame implementation requires no Arrow installation. The optional
[Arrow adapter](frame-nodes.md#arrow-interchange) adds that dependency.

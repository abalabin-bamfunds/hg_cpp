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

Build HG with the web option. Python also requires the Python option:

```sh
cmake -S . -B build/web -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DHG_BUILD_WEB=ON -DHG_BUILD_PYTHON=ON -DCMAKE_INSTALL_PREFIX=install
cmake --build build/web --config Release
cmake --install build/web --config Release
```

Configuration requires libcurl 7.85 or later and Boost 1.87 or later. CMake can
download the required Boost components when a suitable installation is unavailable.
The installation includes `hg_web`, `<hg/web/...>` headers, and the `hg::web` target
in the HG CMake package. Python must be able to import packages from the installation
prefix.

The [C++ example project](../guide/installation.md#c-example-project) uses this target
for web examples:

```cmake
target_link_libraries(example PRIVATE hg::web)
```

### Frame

The frame extension currently builds from its source directory and has a C++ API.
With HG installed, a consuming project adds the frame source and links its target:

```cmake
cmake_minimum_required(VERSION 3.31)
project(example LANGUAGES CXX)
find_package(hg CONFIG REQUIRED)
set(BUILD_TESTING OFF CACHE BOOL "Build tests")
add_subdirectory("${HG_SOURCE_DIR}/extensions/frame" frame)
add_executable(example main.cpp)
target_link_libraries(example PRIVATE hg::frame)
```

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/absolute/path/to/hg/install -DHG_SOURCE_DIR=/absolute/path/to/hg
cmake --build build --config Release
```

The native frame implementation requires no Arrow installation. The optional
[Arrow adapter](frame-nodes.md#arrow-interchange) adds that dependency.

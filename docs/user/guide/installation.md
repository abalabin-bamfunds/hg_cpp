# Installation

HG requires a C++23 compiler and CMake 3.31 or later. Python authoring requires
Python 3.12 or later. The following commands build from a local repository checkout;
configuration downloads build dependencies.

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```sh
python -m venv .venv
```

Activate the environment with `source .venv/bin/activate` on Linux or macOS,
or `.venv\Scripts\Activate.ps1` in Windows PowerShell.

```sh
python -m pip install .
python -c "from hg import graph, run_graph; print('HG is available')"
```
:::

:::{tab-item} C++
:sync: cpp

```sh
cmake -S . -B build/cpp -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX=install
cmake --build build/cpp --config Release
cmake --install build/cpp --config Release
```

The installation contains the `hg::library` CMake target, public headers, and libraries.
:::
::::

(cpp-example-project)=
## C++ example project

Each C++ example is a complete `main.cpp`. This `CMakeLists.txt` builds it against
an installed HG package:

```cmake
cmake_minimum_required(VERSION 3.31)
project(example LANGUAGES CXX)
find_package(hg CONFIG REQUIRED)
add_executable(example main.cpp)
target_link_libraries(example PRIVATE hg::library)
```

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/absolute/path/to/hg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
Shared-library installations may require the installation's library directory on the
platform's library search path. On Windows, add its `bin` directory to `PATH`.
For the SDK included in a Windows wheel, also add `hg_cpp.libs` from the same
installation prefix.

The [example downloads](../how-to/examples.md) include Python source and C++ projects.

## Optional extensions

The [web extension](../extensions/index.md#web) adds HTTP and WebSocket services.
The [frame extension](../extensions/index.md#frame) adds C++ columns and tables.
Their build options and example targets are documented separately.

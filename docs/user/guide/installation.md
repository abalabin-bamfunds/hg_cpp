# Installation

For either Python or C++ development, install the `hg-cpp` distribution with Python
3.12 or later. It includes the Python `hgraph` package together with the C++ SDK's
public headers, libraries, and CMake targets, so both languages use the same installation.

## Install the package

A virtual environment keeps the installation separate from other Python projects:

```sh
python -m venv .venv
```

Activate it with `source .venv/bin/activate` on Linux or macOS, or
`.venv\Scripts\Activate.ps1` in Windows PowerShell.

Install the package in the active environment:

```sh
python -m pip install hg-cpp
```

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

The distribution name is `hg-cpp`; Python code imports from `hgraph`:

```sh
python -c "from hgraph import graph, run_graph; print('HG is available')"
```

[Basic syntax](getting-started.md) contains complete examples.
:::

:::{tab-item} C++
:sync: cpp

C++ applications require a C++23 compiler and CMake 3.31 or later. The pip installation
provides the SDK; building an application does not require an HG source checkout.

(cpp-example-project)=
## C++ example project

To build any of the core C++ examples, save its complete source as `main.cpp` and
use this `CMakeLists.txt` to link it to the installed `hg::library` target:

```cmake
cmake_minimum_required(VERSION 3.31)
project(example LANGUAGES CXX)
find_package(hg CONFIG REQUIRED)
add_executable(example main.cpp)
target_link_libraries(example PRIVATE hg::library)
```

Find the SDK installation prefix in the environment where `hg-cpp` was installed:

```sh
python -c "from importlib.metadata import distribution; print(distribution('hg-cpp').locate_file(''))"
```

Replace `/path/to/site-packages` below with the printed directory:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
On Windows, add the `hgraph`, `bin`, and `hg_cpp.libs` directories under the printed
prefix to `PATH` so the executable can load the wheel's DLLs.

The [example downloads](../how-to/examples.md) include complete C++ projects.
:::
::::

## Optional extensions

If your graph needs HTTP or WebSocket connections, use the
[web extension](../extensions/index.md#web), included in the `hg-cpp` wheel for both
Python and C++. For C++ columns and tables, use the
[frame extension](../extensions/index.md#frame), which currently builds from its
source directory.

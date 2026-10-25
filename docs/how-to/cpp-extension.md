# C++ nodes in Python

To expose a native graph operation to Python, export its operator from a
nanobind module linked to `bg::python`. The same native overloads can then serve
C++ and Python graph code.

Prerequisites: a BG installation built with the Python component, the same
Python environment used to build it, and nanobind. This example exposes the
existing `const_` operator and requires no new scalar conversions.

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

After building `sensor_native`, graph code can import its exported operator:

```python
from bg import graph, run_graph
from sensor_native import constant

@graph
def temperature() -> int:
    return constant(18) + 2

print(run_graph(temperature))
```

Output: `[20]`.
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

The extension source is `sensor_native.cpp`:

```cpp
#include <bg/library.hpp>
#include <bg/python/operator.hpp>
#include <nanobind/nanobind.h>

using namespace bg;
using namespace bg::literals;

NB_MODULE(sensor_native, module) {
    python::export_operator(module, "constant", operators::const_);
}
```
:::

:::{tab-item} Build and run

```cmake
cmake_minimum_required(VERSION 3.31)
project(sensor_native LANGUAGES CXX)
find_package(bg CONFIG COMPONENTS python REQUIRED)
find_package(nanobind CONFIG REQUIRED)
nanobind_add_module(sensor_native NB_SHARED sensor_native.cpp)
target_link_libraries(sensor_native PRIVATE bg::python bg::library)
```

Use the BG installation and the selected environment's nanobind CMake directory
in `CMAKE_PREFIX_PATH`. With `sensor_native.cpp` and `CMakeLists.txt` in the
current directory, configure and build:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install -DPython_EXECUTABLE=/path/to/python
cmake --build build --config Release
```

The BG Python component finds nanobind in the selected Python environment.
Put the module's output directory on `PYTHONPATH` before running the Python
example; multi-configuration generators place it under `build/Release`.
Use the same Python interpreter and compatible compiler as the BG installation.
:::
::::
:::::
::::::

Native registrations must remain valid for all uses. Exporting an operator
observes later registered overloads. Custom scalar types need conversions
registered before Python values are supplied. Callback-scoped scalar views must
not escape a callback. Native code manipulating Python objects must follow the
Python component's GIL and ownership contracts.

[Operator reference](../reference/operators.md) covers selection and registration;
[scalar reference](../reference/scalars.md) covers Python object compatibility.

# Your first graph

Write a calculation that adds a calibration offset to each temperature reading,
then run it with three readings. A single calculation is enough to start:
this example runs a graph with one calculation node.

First, [install BG](installation.md) if you have not installed it yet.

## Write and run the example

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

Save this complete example as `first_graph.py`:

```python
import bg


@bg.graph
def calibrated_temperature(reading: int) -> int:
    return reading + 2


print(bg.run_graph(calibrated_temperature, [18, 20, 19]))
```

Run it:

```sh
python first_graph.py
```

Output:

```text
[20, 22, 21]
```

`@bg.graph` marks a function that constructs a graph. The `int` annotations
describe the input and output value types. Inside the function, `reading`
identifies the changing input. The expression `reading + 2` connects an
addition; BG performs that addition during evaluation.

`bg.run_graph` runs the graph with the supplied input updates and returns a
result containing the output updates.
:::

:::{tab-item} C++
:sync: cpp

Create `examples/first-graph/first_graph.cpp` in the BG repository with this
complete example:

```cpp
#include <bg/node.hpp>
#include <bg/run_graph.hpp>

#include <iostream>

int main() {
    const auto calibrated_temperature = bg::node([](int reading) {
        return reading + 2;
    });

    const auto result = bg::run_graph(calibrated_temperature, {18, 20, 19});
    std::cout << std::boolalpha << result.equals({20, 22, 21}) << '\n';
}
```

Save this alongside it as `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.31)
project(first_graph LANGUAGES CXX)

find_package(bg CONFIG REQUIRED)

add_executable(first_graph first_graph.cpp)
target_link_libraries(first_graph PRIVATE bg::bg)
```

From the repository directory, configure and build:

```sh
cmake -S examples/first-graph -B build/first-graph -DCMAKE_PREFIX_PATH="$PWD/install"
cmake --build build/first-graph --config Release
```

Run on Linux or macOS:

```sh
./build/first-graph/first_graph
```

With a multi-configuration generator such as Visual Studio, add the installed
DLL directory to this PowerShell session and run:

```powershell
$env:PATH = "$PWD/install/bin;$env:PATH"
.\build\first-graph\Release\first_graph.exe
```

Output:

```text
true
```

`bg::node` defines a calculation with plain `int` values. BG calls the lambda
with the current reading during evaluation and publishes its return value.

`bg::run_graph` builds and runs a graph containing that node.
`{18, 20, 19}` supplies the input updates. The runner infers their type from
the plain `int` parameter. `equals` compares the recorded updates with the
expected literal sequence. C++ examples print `true` when the updates match.
:::
::::

## Follow the updates

`run_graph` supplies one reading per evaluation step:

| Step | Reading | Calibrated temperature |
| --- | --- | --- |
| 1 | 18 | 20 |
| 2 | 20 | 22 |
| 3 | 19 | 21 |

Change the offset from `2` to `5` and run the example again. The output updates
become `23`, `25`, and `24`. In C++, update the expected values in `equals` too;
it prints `false` while they differ.

Continue to [Values that change](changing-values.md) to skip an update and
connect two inputs that update independently.

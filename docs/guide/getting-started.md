# A first BG program

To run a small BG calculation, define a graph function and pass it to
`test_graph`. This program returns the fixed value `18`.

BG must be [installed](installation.md) to run the examples.

## Write and run the example

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

This complete example can run as `first_graph.py`:

```python
from bg import graph, test_graph
from bg.operators import const_


@graph
def example() -> int:
    return const_(18)


print(test_graph(example))
```

To run the file:

```sh
python first_graph.py
```

Output:

```text
[18]
```

`from bg import ...` imports graph authoring and testing functions. `@graph`
marks `example` as a graph function. The empty parentheses mean it has no
parameters; `-> int` declares an integer result. `return` identifies its output.

`const_(18)` provides the fixed value to the graph. `test_graph(example)` runs
the graph and returns a list containing its result. It takes the graph function
by name, without calling `example()` here.
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/testing/test_graph.hpp>

#include <iostream>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;

auto example() {
    return const_(18);
}

int main() {
    const auto result = test_graph(example);
    std::cout << std::boolalpha << result.equals({18}) << '\n';
}
```

Output:

```text
true
```

The headers provide library calculations and `test_graph`. The `using namespace`
declarations make their names available without a prefix.

`example` is a graph function. The empty parentheses mean it has no parameters;
`auto` infers its return type. `const_(18)` provides a fixed value, and `return`
identifies the graph's output.

`main` runs the graph with `test_graph(example)`. `equals({18})` checks the
recorded result; `std::boolalpha` prints that check as `true` or `false`.
:::

:::{tab-item} Build and run

Create `examples/first-graph/` in the BG repository and save the code there
as `first_graph.cpp`.

Save this alongside it as `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.31)
project(first_graph LANGUAGES CXX)

find_package(bg CONFIG REQUIRED)

add_executable(first_graph first_graph.cpp)
target_link_libraries(first_graph PRIVATE bg::library)
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
:::
::::

:::::
::::::

For expressions that combine values, [Arithmetic](arithmetic.md) covers
operators and named results.

[Constant sources](../reference/sources.md) specifies the full contract.

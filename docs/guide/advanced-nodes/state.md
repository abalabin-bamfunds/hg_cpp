# State and lifecycle

State retains application data between evaluations. Lifecycle callbacks initialize
and release resources with a graph instance.

## Retained state

Python `State[Factory]` creates one state object per runtime node. C++ `State<T>`
provides access to that node's retained value.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import State, graph, node, run_graph

class Readings:
    def __init__(self):
        self.peak = 0

@node
def peak_above_zero(reading: int, state: State[Readings] = None) -> int:
    state.peak = max(state.peak, reading)
    return state.peak

@graph
def peak(reading: int) -> int:
    return peak_above_zero(reading)

print(run_graph(peak, [18, 17, 21, None, 19]))
```

Output:

```text
[18, 18, 21, None, 21]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>
#include <algorithm>

using namespace bg;
using namespace bg::operators;
using namespace literal;

struct Readings {
    i64 peak{};
};

struct PeakAboveZero {
    In<TS<i64>> reading;
    State<Readings> state;
    Out<TS<i64>> output;

    static constexpr bool deduplicate_wiring = false;

    void evaluate() const {
        state->peak = std::max(state->peak, *reading);
        output.set(state->peak);
    }
};

const auto peak_above_zero = node<PeakAboveZero>();

auto peak(wp<TS<i64>> reading) {
    return peak_above_zero(reading);
}

int main() {
    const auto result = run_graph(graph(peak), {18, 17, 21, SKIP, 19});
    std::cout << std::boolalpha << result.equals({18, 18, 21, SKIP, 21}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

The peak remains `21` after a lower reading. A new run starts with new state.
Required-input invalidation does not reset arbitrary custom state; reset behavior
must be part of the node's contract.

C++ aggregate node fields declare inputs, state, and output. `evaluate` reads the
inputs, updates state, and publishes through `output.set`. Python injects the state
parameter; graph code does not supply it.

## Independent instances

Python stateful nodes use separate state for each call. C++ normally reuses equal
node calls; `deduplicate_wiring = false` requests independent runtime nodes when
independent state is required. This differs from suppressing equal value updates.

Each mapped key owns its graph instance. Removing and re-adding a key creates new
state. A switch starts fresh state when it changes case. The
[per-sensor application](../../how-to/per-sensor-totals.md) demonstrates this lifecycle.

## Start and stop

Start callbacks initialize state or acquire resources. Stop callbacks release those
resources when the instance stops, including an instance selected by `switch_`.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import State, graph, node, run_graph

class Session:
    def __init__(self):
        self.offset = 0

@node
def corrected(reading: int, state: State[Session] = None) -> int:
    return reading + state.offset

@corrected.start
def start(state: State[Session]):
    state.offset = 2

@corrected.stop
def stop(state: State[Session]):
    state.offset = 0

@graph
def temperature(reading: int) -> int:
    return corrected(reading)

print(run_graph(temperature, [18, 19]))
```

Output:

```text
[20, 21]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
using namespace literal;

struct Session {
    i64 offset{};
};

struct Corrected {
    In<TS<i64>> reading;
    State<Session> session;
    Out<TS<i64>> output;

    void start() const { session->offset = 2; }
    void evaluate() const { output.set(*reading + session->offset); }
    void stop() const noexcept { session->offset = 0; }
};

const auto corrected = node<Corrected>();

auto temperature(wp<TS<i64>> reading) {
    return corrected(reading);
}

int main() {
    const auto result = run_graph(temperature, {18, 19});
    std::cout << std::boolalpha << result.equals({20, 21}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

The example uses an offset to expose initialization in the result. Resource-owning
nodes use the same callback boundaries. [State reference](../../reference/state.md)
specifies injection, callback signatures, and cleanup.

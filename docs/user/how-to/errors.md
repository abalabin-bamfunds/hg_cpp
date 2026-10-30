# Errors and diagnostics

A wiring failure means BG could not construct a compatible graph. An evaluation
failure means a callback or library calculation failed while processing values.
The runner propagates either failure after cleanup.

## Type errors

An integer input multiplied by a floating-point scalar has no matching numeric
overload. The error names the operation and the supplied types. Convert the
integer input explicitly when the required result is floating point.

The example prints the first two diagnostic lines, then runs the corrected graph.
The full error also lists the rejected overloads and their reasons.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, graph, run_graph
from bg.operators import convert

@graph
def mixed_types(reading: int) -> float:
    return reading * 0.5

try:
    run_graph(mixed_types, [18])
except ValueError as error:
    print(type(error).__name__)
    print("\n".join(str(error).splitlines()[:2]))

@graph
def scaled(reading: int) -> float:
    return convert[TS[float]](reading) * 0.5

print(run_graph(scaled, [18]))
```

Output (ellipsis):

```text
ValueError
cannot wire operator 'mul': Cannot resolve operator mul at ...
Arguments: (lhs: TS<i64>, rhs: double)
[9.0]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/run_graph.hpp>
#include <bg/graph/wiring_error.hpp>
#include <iostream>
#include <sstream>
#include <string>

using namespace bg;
using namespace bg::operators;
using namespace literal;

auto mixed_types(wp<TS<i64>> reading) {
    return reading * 0.5;
}

auto scaled(wp<TS<i64>> reading) {
    return convert[type<TS<double>>](reading) * 0.5;
}

int main() {
    try {
        (void)run_graph(mixed_types, {18});
    } catch (const WiringError& error) {
        auto lines = std::istringstream{error.what()};
        auto line = std::string{};
        for (auto index = 0; index < 2 && std::getline(lines, line); ++index) {
            std::cout << line << '\n';
        }
    }
    std::cout << run_graph(scaled, {18}) << '\n';
}
```

Output (ellipsis):

```text
Cannot resolve operator mul at ...
Arguments: (lhs: TS<i64>, rhs: double)
[9]
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project)
builds this example as `main.cpp`, linked to `bg::library`.
:::
::::
:::::
::::::

`lhs: TS<i64>` is the integer input; `rhs: double` is the scalar `0.5`.
The integer overload rejects the right operand, and the double overload rejects
the left. `convert` creates the floating-point input required by multiplication.
Changing the declared return type alone does not convert values.
See [numeric conversion](../guide/graphs/types.md#operator-matching-and-conversion) for supported conversions and limits.

The `...` in the Python excerpt represents a generated wiring path. Paths depend on
the surrounding graph. Python exception notes add graph-call context; C++
`WiringError` exposes `diagnostic()`, `contexts()`, and the underlying `cause()`.
Keep the complete diagnostic when reporting a failure.

## Evaluation errors

A zero divisor is a valid integer input to BG. Application code must decide
whether to reject it, publish a fallback, or skip publication. This example
first rejects zero, then implements a policy that skips the result for zero.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, node, run_graph

@node
def ratio(reading: int, divisor: int) -> int:
    if divisor == 0:
        raise ValueError("divisor must be nonzero")
    return reading // divisor

@graph
def normalized(reading: int, divisor: int) -> int:
    return ratio(reading, divisor)

try:
    run_graph(normalized, [18], [0])
except ValueError as error:
    print(f"{type(error).__name__}: {error}")

@node
def safe_ratio(reading: int, divisor: int) -> int:
    if divisor == 0:
        return None
    return reading // divisor

@graph
def safe_normalized(reading: int, divisor: int) -> int:
    return safe_ratio(reading, divisor)

print(run_graph(safe_normalized, [18, None, None], [2, 0, 3]))
```

Output:

```text
ValueError: divisor must be nonzero
[9, None, 6]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <optional>
#include <stdexcept>

using namespace bg;
using namespace bg::operators;
using namespace literal;

const auto ratio = node([](i64 reading, i64 divisor) {
    if (divisor == 0) {
        throw std::invalid_argument{"divisor must be nonzero"};
    }
    return reading / divisor;
});

auto normalized(wp<TS<i64>> reading, wp<TS<i64>> divisor) {
    return ratio(reading, divisor);
}

const auto safe_ratio = node([](i64 reading, i64 divisor) -> std::optional<i64> {
    if (divisor == 0) {
        return std::nullopt;
    }
    return reading / divisor;
});

auto safe_normalized(wp<TS<i64>> reading, wp<TS<i64>> divisor) {
    return safe_ratio(reading, divisor);
}

int main() {
    try {
        (void)run_graph(normalized, {18}, {0});
    } catch (const std::invalid_argument& error) {
        std::cout << error.what() << '\n';
    }
    const auto result = run_graph(graph(safe_normalized), {18, SKIP, SKIP}, {2, 0, 3});
    std::cout << result << '\n';
}
```

Output:

```text
divisor must be nonzero
[9, None, 6]
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project)
builds this example as `main.cpp`, linked to `bg::library`.
:::
::::
:::::
::::::

Skipping publication keeps the previous output value, `9`, during the zero
divisor update. It publishes `6` when the divisor becomes `3`. Use
[invalidation](../guide/advanced-graphs/types.md#validity) instead if a zero divisor must remove the
previous result. Ordinary node control flow checks zero before division;
[value selection](../guide/graphs/conditionals.md#value-selection) would leave
both calculations connected.

Python preserves the original exception type and traceback, including the node
callback that raised it. Wiring exceptions can also carry graph-call notes.
Retain the traceback and any notes in application logs.
See [output publication](../guide/nodes/results.md#skipping-publication) for no-update return behavior.

## Other wiring failures

| Failure | Check | Correction |
| --- | --- | --- |
| Optional argument rejected | Receiver's validity constraint | Declare an optional parameter or supply a fallback |
| Native truth conversion of a connection | `if`, Python `and/or/not`, chained comparison | Use [graph conditionals](../guide/graphs/conditionals.md) |
| Missing collection output type | Generic or untyped signature | Specialize the graph or supply the required result type |
| Missing service implementation | Catalogue and scalar filters | Register one matching implementation |
| Dependency cycle | Connections or mesh dependencies | Make same-cycle dependencies acyclic |

Start with the innermost failed operation and its supplied argument types.
Read candidate rejection reasons before changing a signature.

## Missing or unexpected updates

An invalid required input normally prevents evaluation; it does not raise an
exception. A skipped test position preserves a previous value. Invalidation
removes it. [Testing](testing.md) records those events without application sources.

- Check whether all required inputs have current values.
- Check which inputs schedule evaluation and which merely retain values.
- Check deduplication, selected inputs, filter conditions, sampling, and timers.
- Check whether a collection update affects the subscribed key or item.
- Check the run's exclusive stop boundary and whether its source is finite.

[Changing-input patterns](../guide/graphs/updates.md) describes publication rules.
If a failure occurs after a callback, check [view lifetime](../reference/input-views.md).
Python lazy views expire; materialize owned data before retaining it.

# Invalidate and recover

To remove a current reading, publish `INVALID` rather than skipping an update.
Use `valid` when downstream graph code needs to observe whether a value exists.

Prerequisites: [Optional inputs](optional-inputs.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import INVALID, graph, test_graph
from bg.operators import valid

@graph
def has_temperature(reading: int) -> bool:
    return valid(reading + 2)

print(test_graph(has_temperature, [None, 18, None, INVALID, 19]))
```

Output:

```text
[False, True, None, False, True]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

auto has_temperature(wp<TS<i64>> reading) {
    return valid(reading + 2);
}

int main() {
    std::cout << std::boolalpha << (test_graph(has_temperature, {SKIP, 18, SKIP, INVALID, 19}).equals({false, true, SKIP, false, true})) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [first program's CMake project](getting-started.md#write-and-run-the-example)
builds this example as `first_graph.cpp`, linked to `bg::library`.
:::
::::
:::::
::::::

| Step | Reading event | Current reading | Calibrated result | Validity update |
| --- | --- | --- | --- | --- |
| 1 | No update | No current value | No current value | False |
| 2 | 18 | 18 | 20 | True |
| 3 | No update | 18 | 20 | No update |
| 4 | Invalidation | No current value | No current value | False |
| 5 | 19 | 19 | 21 | True |

`valid` evaluates at graph start, including before any reading. It publishes
again only when validity changes. Arithmetic requires its input: invalidating
the reading invalidates the calibrated result, and a new reading permits it to
calculate again.

## Preserve, remove, or replace

Skipping an input update preserves its current value. Returning no node output
update preserves the node's current output. Invalidation removes a current
value. An absent scalar argument is fixed during construction; it is not an
invalidation event.

Python's root test result records both invalidation and no update as `None`.
The example observes validity explicitly to distinguish them. C++ literal
comparisons can distinguish `INVALID` from `SKIP`.

`default_(value, fallback)` supplies a result while `value` is unavailable and
follows it when available. It does not make the source valid. See
[selection and validity reference](../reference/library-selection.md).

For node code that must inspect validity before reading, use an
[optional input view](view-validity.md).
